# 渲染管线

> 状态：**一帧的实际路径已经固定**，本文写的是**代码现在怎么跑**，不是计划。
> 对应实现：`core/framework/ui.cpp`、`rhi/d3d12/*`、`core/renderer/shaders/*`、
> `core/text/*`。取舍的**理由**（为什么自研 RHI、为什么不 MSAA）见
> [架构 §3](architecture.md)；本文补的是「这些结论翻译成了什么代码，代价落在哪」。

---

## 一帧从头到尾

```
Ui::paint(device)
  └─ collectPaint(root)              按树序深度优先遍历 Element 树
       ├─ 折叠 clip                  clip 容器把 scissor 往下传
       ├─ ×scale                     逻辑单位 → 物理像素（只在这里做一次）
       ├─ 圆角矩形 → rhi::SdfRect
       └─ 文本 → 每个字形一个 rhi::GlyphQuad（原点吸附到整像素）
  └─ submitRuns(device)              按 DrawRun 顺序提交，一次 run 一次 draw
       ├─ device.setGlyphAtlas(...)  仅当本轮真的光栅化了新字形
       ├─ device.drawSdfRects(run)
       └─ device.drawGlyphQuads(run)

D3D12Device::beginFrame → 录制命令 → endFrame → Present
```

这里有两处是**刻意的**，也是所有渲染取舍的源头：

1. **paint 只把树翻译成图元，不做任何 GPU 对象的创建。** 缓冲、纹理、
   管线都在 device 初始化时建好，之后常驻。这就是「retained」在渲染侧的落地。
2. **逻辑单位只在 paint 里乘一次 `scale`。** 布局、命中测试、事件坐标全是逻辑单位，
   物理像素只存在于 `rhi::SdfRect` / `rhi::GlyphQuad` 之后。

---

## Retained 到底保留什么

「retained mode」经常被说成一句口号，落到代码它是**三层不同粒度的保留**：

| 保留物 | 生命周期 | 变化时的代价 | 代码位置 |
|--------|----------|--------------|----------|
| GPU 资源（实例环形缓冲、R8 图集、PSO、根签名） | 与 device 同寿 | 只在初始化时建一次 | `rhi/d3d12/*.cpp` |
| DrawRun 列表（同类型图元的连续段） | 每次 `paint()` 重建 | 只是往 `std::vector` 里追加 | `Ui::rects_/quads_/runs_` |
| Command Buffer（本帧的提交序列） | 每帧重建 | 便宜的引用 + 常量 | `D3D12Device::beginFrame` |

**结构没变的一帧里，第一条代价是零。** 示例自己的 stats 里
`glyphsRasterized` 会一直平着不动，就是这个性质在数字上的表现。

代价那一面也要说清楚：`rects_` / `quads_` 每帧重建一次（`paint()` 开头
`clear()`）。这没问题，因为它们是 POD，`clear()` 只复位 size；但如果将来一帧的图元
数上万，这里是第一批要改成常驻 + 脏生成的地方（M1 的 DrawBatch）。

---

## 两个管线，没有第三个

今天只有**两条 GPU 管线**，各自一个 shader：

| 管线 | 图元 | 顶点来自 | 片元 |
|------|------|----------|------|
| SDF 圆角矩形 | 一个实心（可圆角、可半透明）矩形 | `SV_VertexID` 程序化生成 quad | 解析 SDF + 覆盖率 |
| 字形 | 一个 R8 覆盖位图采样 quad | 同上 | 图集采样 × 文字颜色 |

**没有**通用路径管线、没有画线、没有渐变、没有阴影、没有图片纹理。
第一版的目标是覆盖「圆角矩形 + 文本」这 90% 的界面，其余进 M1。

这也解释了 `controls.cpp` 里两个看起来很奇怪的做法：

- **1px 描边 = 两个矩形。** 没有 stroke 图元，边框就是「画一个稍大的矩形当边框色，
  再画一个内缩 1px 的矩形当填充」。描边自身的 AA 因此是**两个 SDF 的边缘**，
  在圆角处和内缩量不完全等价于真正的描边 —— 已知，接受。
- **圆点/圆角用 `radius ≥ min(w,h)/2` 表示。** 着色器里 `r = min(radius, min(halfSize))`，
  所以 `radiusPill = 999` 就是胶囊/圆。

---

## 批处理：实例流 + 程序化 quad

矩形和字形都走**实例化绘制**：几何由 `SV_VertexID` 现算（`DrawInstanced(6, count)`，
6 个顶点两个三角形），每个图元是一条 80 字节的实例数据。

```
根签名（两条管线同构）
  b0  32-bit 根常量  = { viewport.xy, instanceBase(uint), pad }
  t0  StructuredBuffer<Instance>   ← 整个环形缓冲一个 SRV
  字形管线另加 t1 = 图集、s0 = 线性采样器
```

关键设计：**实例环形缓冲只建一个 SRV 描述符**（字形管线再给图集加一个 SRV
和一个静态采样器），本帧用哪一段由 `instanceBase` 根常量选。所以描述符在 device
生命周期里只创建一次，**永远不需要重写**。

```cpp
// 每帧的槽位 = frameIndex * kMaxInstancesPerFrame + used
const uint32_t base = frameIndex * kMaxInstancesPerFrame + used;
```

几个必须在脑子里记住的**硬上限**（超了就报 stderr 并截断，不会崩）：

| 限制 | 值 | 位置 |
|------|----|------|
| 每帧 SDF 矩形数 | 4096 | `D3D12SdfBatch::kMaxInstancesPerFrame` |
| 每帧字形 quad 数 | 8192 | `D3D12TextBatch::kMaxInstancesPerFrame` |
| 同时在飞的帧 | 2 | `kFramesInFlight` |
| 交换链缓冲 | 最多 3 | `D3D12Device::kMaxBuffers` |

### 环形的代价：必须等 GPU

实例缓冲是 **upload heap + 持久映射**，没有每帧分配。代价是写入前必须确认
GPU 已经用完这一帧两帧前的那一段：

```cpp
void D3D12Device::beginFrame(Color clear) {
    waitForFrame(frameIndex_);   // fenceValues_[slot] 完成才继续
    ...
}
```

`waitForFrame` 是**有界**等待（fence 值，不是 `Sleep`），空闲时通常立即返回。

---

## 抗锯齿：解析 SDF，明确拒绝 MSAA

UI 的主体是 1px 直线、小字号文本、圆角 —— 这些恰好是 MSAA 表现最差的东西：
MSAA 只在几何边缘做覆盖采样，圆角矩形的一条边只有一次采样，1px 线直接糊；
而且 MSAA 会按倍放大带宽和显存。所以这里走**解析覆盖**。

顶点着色器把 quad 每边**外扩 1 像素**，让 AA 的过渡带落在几何内部：

```hlsl
float2 halfSz  = inst.halfSize + 1.0;
```

片元着色器算到圆角矩形的**有符号距离**（负=内部，单位像素），再映射成覆盖率：

```hlsl
float sdRoundRect(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}
float coverage = saturate(0.5 - d);   // 边缘 ±0.5px 内线性过渡
```

**这是取舍，不是精确的盒式滤波**：`saturate(0.5 - d)` 是线性斜坡，不是真实的
解析覆盖率；直角处用 `min(max(q.x,q.y),0)`，所以角上的覆盖是方形近似而不是
圆角精确值。视觉上在 1px 量级差异不可见，代价是几乎零成本 —— 收益是
**完全不依赖 MSAA，也不需要超采样**。M1 的贝塞尔路径会换成 Skia 式解析覆盖
或 compute 光栅（见 [方案](plan.md) D12）。

文本**不做几何 AA**：字形位图自带 alpha 覆盖，直接乘颜色 alpha。用的是灰度 AA，
不用 subpixel（与透明/旋转/缩放不兼容，且现代趋势如此）。

---

## 裁剪：跟着图元走，不在 device 上

一条 instanced draw call 覆盖整帧，所以**没有办法用 scissor 状态栈** —— 那会
把一次 draw 硬切成多次。于是裁剪被放进**每个实例自己的数据里**：

```hlsl
if (i.clipOn > 0.5) {
    float2 p = i.pos.xy;
    if (p.x < i.clip.x || p.x >= i.clip.z ||
        p.y < i.clip.y || p.y >= i.clip.w) discard;
}
```

代价与结果都要如实记：

- **代价**：每个实例多 20 字节（`clip[4]` + `clipEnabled` + padding）。
- **结果**：被裁的边是**硬边，没有 AA** —— 因为它按片元中心判断，等价于硬件 scissor
  的行为。这对滚动视口正好合适（内容被视口切掉，不需要渐变）。
- **契约**：`rhi::ClipRect` 是半开区间 `[x0,x1)×[y0,y1)`，和 D3D 的 scissor 一致。
- **两层语义**：`ui.cpp` 在遍历时把嵌套 clip **折叠成一个最终矩形**再发给图元，
  所以着色器不需要知道树的形状。M2 的 DrawBatch 会把 clip 还原成设备状态，
  那时候这个「每图元 clip」会被撤掉。

绘制侧的 clip 由 `controls_gallery --paint-test` 用一个**记录型 `rhi::Device`**
断言：视口里每条图元都带着正确的 scissor，且没有一条带着意外的 scissor。

---

## 绘制顺序：树序即画家算法，一律不排序

**没有深度缓冲、没有 z-index、没有按材质重排。** 谁先被 `collectPaint` 走到，
谁就先提交；后来的盖住先来的。

因为矩形和字形是两个独立缓冲，`Ui` 用一份 **DrawRun 列表**记录「一段同类型图元」，
提交时按树序交错：

```cpp
struct DrawRun { bool text; uint32_t begin; uint32_t count; };
```

这样「一个较早的文本节点被一个较晚的矩形盖住」才成立。M0 早期是**两个整批**
（先所有矩形、再所有字形），于是任何压住文本的矩形都错位 —— 这是被
`controls_gallery --paint-test` 抓出来并修掉的（记录型 Device 直接断言交错顺序）。

代价：一次 run 就是一次 draw call。文本-矩形-文本这样的交替会被切成三段；
如果将来 UI 里矩形和文本大量交替，需要引入真正的 DrawBatch 按材质排序
（但那会破坏树序渲染，是 M2 要正面解决的问题）。

---

## 文本：在 reconcile 里 shape，在 paint 里只是发 quad

文本是 CPU 成本最大的一块，所以**所有 shaping 和字形查找都发生在 reconcile**，
paint 只做「读缓存、发实例」。一帧稳态文本工作量为零。

### 三个阶段

```
reconcile（只在字符串/字号/scale 变化时）
  TextSystem::shape        逻辑字号 → ShapedLine（glyph index + face + 小数 advance）
  GlyphAtlas::get          (face, glyph, sizeBucket) → 图集槽位，未命中才光栅化
  → RenderObject::placed   节点局部坐标（物理像素）的字形矩形 + UV

paint（每帧）
  origin × scale，字形原点吸附整像素，发 rhi::GlyphQuad
```

### 为什么字形坐标是「节点局部」

`PlacedGlyph::bounds` 存的是**相对节点左上角**的物理像素位置，不是屏幕坐标。
于是布局把文本节点整体平移时**不需要重新 shape** —— paint 加一次最终原点即可
（`core/framework/element.cpp` 的 `translateSubtree` 就靠这一条，只改矩形不改字形）。

**基线与 advance 用小数**（排版正确），但**字形位图原点在 paint 时吸附到整物理像素**
（`std::round(origin.x + g.bounds.x)`）。原因很直接：把一张位图按小数位置贴上去，
线性采样器会重采样它，UI 文字就糊了。同一条基线上相邻字形的相对偏移恒为整数，
所以吸附**不会改变字距**。

### DPI：逻辑排版，物理光栅化

```
布局 / advance        fontSize            逻辑单位
字形光栅化            fontSize × scale     物理像素   ← 关键
placed 坐标           逻辑 advance × scale + bearing   物理像素（节点局部）
paint                 origin × scale         物理像素
```

**必须在物理尺寸上光栅化**，而不是把小位图放大 —— 后者就是没有 DPI 感知时
Windows 对整个窗口做的事。`RenderObject::rasterScale` 记录了当时用的 scale，
窗口换到另一台显示器（scale 变了）会触发重新 shape，而不是复用旧位图。

---

## 字形图集

单页 **R8 覆盖图集，1024×1024**（`rhi::kGlyphAtlasSize`），shelf 打包，
固定 1px 间隙。几条硬规则：

| 规则 | 为什么 |
|------|--------|
| UV 用**边界到边界**（`x/size … (x+w)/size`），不是纹素中心内缩 | quad 正好跨 `w` 个像素；内缩会把 `w` 个纹素塞进 `w` 个像素，每个像素都双线性混合邻居 → **系统性发软**。边界到边界才是严格 1:1 |
| 1px gutter | 线性采样在字形边缘摸到邻居时，要摸到透明而不是另一个字形 |
| 空字形（空格/制表）也缓存，但不占像素 | 度量要能命中缓存，不能每次重新光栅化 |
| 桶化 size（整物理像素） | 每个小数尺寸一条缓存会爆炸，视觉上没有收益 |
| 缓存键 = `(face, glyphIndex, sizeBucket)` | glyph index 只在**字体内部**唯一，两个字体 42 号字形绝不同槽 |

**已知限制（都是 M0 边界）：** 没有淘汰策略，图集满了 `get` 返回 `null`、
字形被丢弃并计入 `rejected`；只有一页。M1 加 LRU 淘汰与多页。

### 上传只在真的新增字形时发生

图集每帧整张上传是 1 MiB memcpy + 全纹理拷贝。所以：

```cpp
// element.cpp：只有 rasterized 计数变了才标脏
if (ctx.atlasDirty && ctx.atlas->stats().rasterized != rasterizedBefore) {
    *ctx.atlasDirty = true;
}
```

早期版本是**无条件标脏**，于是每次 rebuild 都整张重传（实测 32 次）。现在稳态
是零次上传，`atlasUploads` 也不再随 rebuild 增长。这是「先记数字再优化」的样本：
缺陷是 `stats` 发现的，不是肉眼发现的。

---

## 文本整形：DirectWrite 今天，HarfBuzz/ICU 在 M1

当前整套文本栈是 DirectWrite，接口（`text::TextSystem`）就是为将来替换留的：

```
shape()      逻辑字号 → ShapedLine（glyph index + face + advance）
rasterize()  (face, glyphIndex, 物理字号) → 8-bit 覆盖位图
```

两个**必须记住**的 DirectWrite 事实（都写进了源码注释）：

1. **`GetGlyphIndices` 要 UTF-32，不是 UTF-16。** 把 `wchar_t*` 直接 reinterpret 成
   `UINT32*` 会把两个代码单元当成一个码点，静默返回 `.notdef`。所以有
   `utf16ToUtf32`。
2. **`GetAlphaTextureBounds` 在纹理类型与渲染模式不匹配时返回空矩形，不报错。**
   实测 Segoe UI 上所有 `ALIASED_1x1` 组合都返回空，只有 `CLEARTYPE_3x1` 出墨。
   所以「工作组合」排第一，其余留作回退。后果是**灰度是 ClearType 三通道平均而来的**，
   与原生的灰度 AA 不完全等价（ClearType 自己有一套对比度/gamma 调整）。

字体回退走 `IDWriteFontFallback::MapCharacters`，按 DWrite 返回的 run 逐段解析。
**face 的稳定身份是 `家族名|weight|style|stretch`**，不是 `IDWriteFont*` 指针 ——
`MapCharacters` 返回的指针不保证跨调用稳定，按指针做键会每次 rebuild 都造一个新 face，
把字形缓存彻底打穿。

当前文本栈的**故意限制**：码点→字形 1:1（无连字、无复杂文种整形）、单行不换行、
无 bidi、无 IME。替换点是 M1。

---

## 帧生命周期与交换链

```
beginFrame(clear)
  1. waitForFrame(frameIndex)      等两帧前用完环形缓冲槽
  2. 重置本帧实例游标（batch_.beginFrame / text_.beginFrame）
  3. 重置 allocator + command list
  4. 后缓冲 COMMON/PRESENT → RENDER_TARGET 屏障（首次必须从 COMMON 出发）
  5. OMSetRenderTargets + ClearRenderTargetView
  6. RSSetViewports / RSSetScissorRects        ← 不设视口则什么都光栅不出来
  （这一段由 drawSdfRects / drawGlyphQuads 写入命令）
endFrame()
  1. RENDER_TARGET → PRESENT 屏障
  2. Close + ExecuteCommandLists
  3. Present(vsync ? 1 : 0)
  4. Signal fence，frameIndex 前进
```

几个已经踩过的坑，都写在 `d3d12_device.cpp` 的注释里：

- **交换链后缓冲初始状态是 COMMON**，不是 PRESENT。第一个屏障的 `StateBefore`
  必须写 COMMON，否则调试层报错。用 `firstUse_[idx]` 记这件事。
- **`ResizeBuffers` 前必须释放所有后缓冲引用**（`releaseRenderTargets()`），
  否则 `ResizeBuffers` 返回 `DXGI_ERROR_INVALID_CALL`。
- **`MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)`**：把 resize 的所有权
  从 DXGI 手里拿回来。
- **帧循环不给 Present 之外的节拍**。曾经渲染线程用
  `condition_variable::wait_for(7ms)` 轮询尺寸，被系统定时器粒度放大到
  ~15.6ms，整个应用被静默压到 60Hz（m0 的 frame/paint 断言当场抓住）。现在
  **空闲开 vsync、尺寸变动时关 vsync**，由 Present 自己配速。

---

## 截图与帧回读

`captureFrame()` 把后缓冲 `CopyTextureRegion` 到一张 READBACK 堆的缓冲；
`readbackFrame()` 映射、按行拷贝、去 pitch。细节：

- **行距必须 256 字节对齐**，所以缓冲的 rowPitch 是 `(width*4+255) & ~255`。
- **格式是 `B8G8R8A8_UNORM`**，正是 WIC PNG 编码器直接接受的原生顺序，
  **全程零 swizzle**。
- 拷贝发生在 `beginFrame` 之后、`endFrame` 之前，且在所有绘制之后；
  读回前 `waitForGpu()`。

```
截图（golden-image）路径：
  --capture out.png → host Control::captureNextFrame → 渲染线程 captureFrame()
  → readbackFrame → tools::writePng
```

配合 `host::Options::uiScale = 1.0f`（渲染与 DPI 无关），同一份代码在任何显示器上
产出相同的像素 —— 这是基准图可复现的前提。

---

## 校验层：会静默的东西不止一处

`_DEBUG` 下开 D3D12 调试层，`InfoQueue` 过滤掉 INFO 严重级，每帧由
`reportDiagnostics()` 排空并打到 stderr（`[d3d12:warning] …`）。

但**校验层不是万能的**。最典型的一条：把 `instanceBase` 当 `float` 传进声明为
`uint` 的根常量，位模式被重新解释 → 越界读到 0 → 几何整体退化。
**调试层完全静默**，只有帧回读的截图能发现。所以这个项目里
「能出图 + 能对像素做断言」是基建，不是锦上添花。

`reportDiagnostics()` 在没有 `_DEBUG` 时是空操作，不影响发布构建。

---

## 还没做的渲染项

和「已做」一样重要的是把「没做」列清楚，避免误以为已经支持：

| 项 | 现状 | 计划 |
|----|------|------|
| 贝塞尔 / 任意路径 AA | 无 | M1（D12 要求） |
| 图片 / 渐变 / 阴影图元 | 无 | M1 |
| Device lost 恢复 | 无 | M1 |
| 字形图集淘汰 / 多页 | 无（满了丢字形） | M1 |
| DrawBatch（按材质排序、clip 还原为状态） | 无，DrawRun + 逐图元 clip | M2 |
| MSDF / subpixel AA | 明确不做 | —— |
| 多后端（Vulkan/Metal） | 无，仅 D3D12 | 二期（D11） |

---

## 接着读

- [框架实现](framework.md) —— reconcile、override、布局、输入路由
- [踩坑与注意事项](gotchas.md) —— 上述每一条的「症状 → 根因 → 修法 → 教训」
- [架构 §3](architecture.md) —— 为什么 retained、为什么解析 AA、为什么自研 RHI
- [方案](plan.md) §6.1 —— M0 验收标准与实施进度
