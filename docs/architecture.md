# TacUI 架构方案

> GPU 加速 / Retained Mode / C++ 核心 / 多语言声明式 DSL 的 UI 库
> 参考 GacUI 的控件与组合思想，但放弃 XML + 脚本资源，改用各宿主语言原生的代码式声明 UI

---

## 0. 目标与非目标

### 目标
| # | 目标 | 说明 |
|---|------|------|
| G1 | GPU 加速渲染 | 光栅化、合成、动画插值全部走 GPU |
| G2 | Retained mode | 保留视觉树 + 保留 GPU 资源，属性变化只更新 uniform / transform，不重建 GPU 对象 |
| G3 | C++ 核心 | 无宿主语言依赖的独立 core，可嵌入任何进程 |
| G4 | 多语言绑定 | Rust / C# / Python / Swift / Kotlin / TS 等，由稳定的 C ABI + IDL 代码生成 |
| G5 | 代码式声明 UI | 无 XML、无独立脚本语言。各宿主语言用其原生语法表达 UI |
| G6 | 可嵌入 | 能作为库嵌进已有应用（游戏引擎、桌面 App、插件宿主） |

### 明确非目标（第一版）
- 不做即时模式（Immediate mode）API
- 不做跨平台移动端（Windows 优先，Linux/macOS 次之）
- 不做 HTML/CSS 兼容层
- 不追求 100% 像素级还原任何现有框架

---

## 1. 整体分层

```
┌──────────────────────────────────────────────────────────────┐
│ L5  DSL / 宿主语言层                                          │
│    Rust proc-macro │ C# source-gen │ Python builder │ TS WASM │
│    产出：VNode 构建调用序列（紧凑、批量、跨 FFI）                │
├──────────────────────────────────────────────────────────────┤
│ L4  Binding 层  (唯一的稳定 ABI 边界)                         │
│    C ABI + IDL 生成 · 句柄代际索引 · 回调表 · 版本协商          │
├──────────────────────────────────────────────────────────────┤
│ L3  Framework 层 (C++)                                        │
│    Element 树 (diff + state) │ RenderObject 树 (layout/paint)  │
│    控件库 · 样式/主题 · 动画时钟 · 输入路由 · 数据绑定            │
├──────────────────────────────────────────────────────────────┤
│ L2  Scene / Retained Layer (C++)                              │
│    Layer 树 · DrawBatch · GPU 资源句柄 · 脏标记 · 属性快照       │
├──────────────────────────────────────────────────────────────┤
│ L1  Renderer (C++ / RHI 抽象)                                 │
│    批处理 · Analytic AA · Glyph Atlas · 矢量路径 · 合成器        │
├──────────────────────────────────────────────────────────────┤
│ L0  Platform / RHI (C++)                                      │
│    D3D12 · Vulkan · Metal  │ 窗口 · 输入 · IME · DPI · 无障碍   │
└──────────────────────────────────────────────────────────────┘
```

**关键约束：L4 是唯一跨语言边界。** L2/L3 永远不直接暴露给宿主语言。

---

## 2. 核心架构：三棵树（参考 Flutter，而非 WPF）

这是整个方案最重要的决策。WPF 的 `Visual` / `DependencyObject` 模型有一个致命问题：**它把"声明"和"状态"和"布局"揉在一个可变对象里**，导致 diff 无法做、跨语言传递必须全量复制。

Flutter 的三棵树模型是这个问题目前最成熟的答案：

| 树 | 生命周期 | 谁持有 | 职责 |
|---|---------|-------|------|
| **VNode / Widget** | 每次状态变化重建（不可变值） | 宿主语言生成，传给 core | 纯描述：类型 + 属性 + 子节点 |
| **Element** | 跨帧稳定（可变，位置决定身份） | core | diff 结果 + **state 槽** + 生命周期回调 |
| **RenderObject** | 跨帧稳定 | core | layout（Measure/Arrange）+ paint（产出 DrawBatch） |

### 2.1 diff 算法
```
reconcile(oldElement, newVNode):
  if old.runtimeType != new.runtimeType or old.key != new.key:
      -> 卸载整棵子树，mount 新的
  else:
      old.updateProps(newVNode.props)   // 只更新差异属性
      reconcileChildren(old, newVNode.children)  // 按 key / 按位置匹配
```

节点身份 = `路径(父 Element 的 id) + key(可选) + 位置`。
有 `key` 时按 key 匹配（列表重排序），无 key 时按位置匹配。

### 2.2 state 槽
`Element` 上挂一个 `StateSlot[]`，按**位置索引**寻址——和 SwiftUI `@State` 的机制一致。
宿主语言的 `state(initialValue)` 在 core 侧注册一个槽位，读写通过 handle 走 FFI。

```
// 伪代码
let count = use_state(0);        // -> StateHandle<i32>，绑定到当前 Element 的第 0 号槽
count.set(count.get() + 1);      // -> core 标记该 Element 子树 dirty -> 调度重建
```

### 2.3 重建的边界（性能关键）
**不要每帧重建 VNode 树。** 只有 state 变化的那棵子树重建，且重建后立即 diff。
动画参数变化走 **L2 的属性直写通道**，完全绕过 diff：

```
state 变化 -> 重建 VNode 子树 -> diff -> Element.updateProps -> RenderObject 标记重布局/重绘
动画 tick  -> RenderObject.setAnimValue()  -> 只改 uniform，不碰 VNode/Element
```

---

## 3. 渲染架构（Retained，必须做对的地方）

### 3.1 "Retained" 到底保留什么

三层保留，粒度不同：

| 保留物 | 是否跨帧保留 | 变化时的代价 |
|-------|------------|------------|
| GPU 资源（顶点缓冲、纹理、glyph atlas 页） | ✅ 长期保留 | 结构变化才重建 |
| DrawBatch（一批同材质同 clip 的绘制项） | ✅ 结构变化才重建 | 属性变化只改 uniform buffer |
| Command Buffer（每帧的提交序列） | ❌ 每帧生成 | 便宜，只是引用 batch + push uniform |

**这是 WPF 桌面合成和 Win2D/DirectComposition 的差别所在。** WPF 每帧重新光栅化 display list；我们要做的是每帧只生成 command buffer + 提交常量，几何和纹理都在 GPU 侧常驻。

### 3.2 DrawBatch 与批处理
```cpp
struct DrawBatch {
    PipelineHandle pipeline;   // 决定材质/shader
    BufferHandle   vertices;   // 常驻
    BufferHandle   indices;
    TextureHandle  texture;    // atlas / 图片
    Rect           clip;       // scissor 或 stencil 引用
    uint32_t       uniformOffset;
};
```
排序：`Opaque -> AlphaTest -> AlphaBlend`，同材质同 clip 合并。
圆角矩形 / 圆角 clip 走 **SDF fragment shader**，不切几何、不用 stencil。

### 3.3 反锯齿（最容易翻车的一环）
不要指望 MSAA。UI 的主体是 1px 直线、小字号文本、圆角——MSAA 只会让它们糊掉。

分三类处理：

| 图元 | AA 方案 |
|-----|--------|
| 矩形 / 圆角矩形 / 边框 | fragment shader 里算解析覆盖率（SDF，几乎零成本，质量最高） |
| 文本 | 字形位图自带 alpha，不需要几何 AA。灰度 AA（不用 subpixel） |
| 任意路径（图标、曲线） | Analytic coverage（Skia Analytic AA）或 compute raster（Vello 思路） |

**第一版可以只支持圆角矩形 + 文本 + 预栅格化图标**，任意路径放二期。这能覆盖 90% 的 UI。

### 3.4 文本渲染（工作量最大的一块）
```
HarfBuzz (shaping)  ->  ICU (bidi + line breaking)  ->  Rasterizer  ->  Glyph Atlas
                       平台 fallback: DirectWrite / CoreText / fontconfig
```
- **灰度 AA + R8 atlas**，不用 MSDF（CJK 字形量太大、细节丢失），不用 subpixel（与透明/旋转/缩放不兼容）
- 字形缓存：`(font, size, glyphId)` -> atlas 槽位，LRU 淘汰，按尺寸分桶避免污染
- **文本测量是最大的 CPU 瓶颈**：必须缓存 `(text, font, constraints) -> metrics`，且**异步 shaping**（大段文本首帧先出占位，shaping 完再重排）

### 3.5 RHI 选择
| 方案 | 优点 | 缺点 |
|-----|-----|-----|
| **自研薄抽象 + D3D12/Vulkan/Metal 后端** | 完全控制，可做 RHI 级别的优化 | 工作量巨大（3 个后端 × 各平台的坑） |
| **Dawn (WebGPU)** | 一套 API 覆盖三平台，C++ 友好，Google 维护 | 偏重，某些低层能力受限 |
| **bgfx / sokol** | 轻量 | 抽象层次不合适，2D 优化空间小 |

**推荐：自研 RHI 接口 + 第一版只做 D3D12 后端**，Vulkan/Metal 二期。理由：UI 渲染对 RHI 的要求很窄（几个 pipeline、动态 uniform、纹理更新、scissor），自研抽象成本可控，且能针对 UI 做特化。

---

## 4. 布局系统

采用 **Measure/Arrange 两遍布局**（WPF），它天然兼容 SwiftUI 的 `propose -> return` 语义：

```cpp
Size Measure(Size availableSize);      // 父提议 -> 子返回 desired size
void Arrange(Rect finalRect);          // 父给定最终矩形
```

映射关系：
| SwiftUI | TacUI |
|---------|-------|
| `frame(width:height:)` | `ConstrainedBox` |
| `padding()` | `Padding` |
| `HStack/VStack` | `StackPanel(orientation)` + `MainAxisAlignment` / `CrossAxisAlignment` |
| `Spacer()` | `Expander`（弹性尺寸） |
| `.overlay/.background` | `Overlay` / `Background`（独立 layout 通道） |

要点：
- **增量布局**：脏标记向上传播 `invalidateMeasure`，只重排受影响的分支
- **布局缓存**：期望 size 缓存在 RenderObject 上，未脏时直接命中
- 不做 GPU 布局，布局在 CPU 上跑，目标是 < 2ms

---

## 5. 跨语言绑定

### 5.1 边界设计原则
C ABI 是唯一出口。硬性规则：
- ❌ 不跨边界抛异常（core 内部用异常，边界处 catch 并返回 error code）
- ❌ 不跨边界传 STL 类型 / 模板 / 含虚函数的 C++ 类
- ✅ 字符串用 `(const char* ptr, size_t len)`，UTF-8
- ✅ 所有对象用**不透明句柄 + 代际索引**（generational index）防悬垂：
  ```c
  typedef struct { uint32_t index; uint32_t generation; } TuiHandle;
  ```
- ✅ 结构体固定布局，显式 padding，加 `static_assert` 校验 sizeof
- ✅ 版本号 + 能力查询 `tui_get_abi_version()` / `tui_has_feature()`

### 5.2 减少跨界次数的关键设计
UI 是回调密集的，逐属性 FFI 调用会吃掉所有收益。三个手段：

1. **调用序列化 / 批处理**：宿主构建 VNode 时不是每个属性一次 FFI，而是一次调用传一个紧凑的编码块
   ```c
   tui_vnode_begin(ctx, TYPE_VSTACK, node_key);
   tui_vnode_props(ctx, packed_props, len);   // 一次传所有属性
   tui_vnode_end(ctx);
   ```
   更激进的做法：宿主侧组装 FlatBuffers 风格的二进制，一次 `tui_commit_subtree(ctx, buf, len)` 提交整棵子树。
   > 实测经验：普通界面几百个节点，逐属性调用也就几千次，约 10-50µs，可接受。**先做简单的，等 profiling 说不行再优化。** 只有虚拟化长列表才必须批量。

2. **回调表 + userdata**：core -> 宿主的所有回调通过一张函数指针表注册，而不是每次注册单独的回调
   ```c
   typedef struct {
       void (*on_click)(void* ud, TuiHandle h);
       void (*on_text_input)(void* ud, const char* utf8, size_t len);
       void* userdata;
   } TuiHostCallbacks;
   ```

3. **状态驻留位置**：state 存在 core（Element 槽位），宿主通过 handle 读写。避免宿主自己维护一份状态再同步进 core。

### 5.3 IDL 与代码生成
```
idl/tacui.idl
    ├─> cbindgen          -> tacui.h (C header)
    ├─> rust-bindgen      -> Rust FFI 层
    ├─> source generator  -> C# P/Invoke + 强类型 wrapper
    ├─> cffi/pybind11     -> Python
    ├─> JNI               -> Kotlin/Java
    └─> WASM + ts-dts     -> TypeScript
```
不要手写六份。IDL 从第一天就要有，但可以先用 `cbindgen` 从 C header 反推，等 API 稳定再抽 IDL。

### 5.4 各语言的推荐方案
| 语言 | 绑定方式 | DSL 方式 | 备注 |
|-----|---------|---------|------|
| **C++** | 直接链接 | Fluent builder + 可选宏 | 一等公民，头文件库风格 |
| **Rust** | bindgen / 手写 safe wrapper | **proc-macro `view!{}`** | 体验最好，建议优先 |
| **C#** | P/Invoke 或 C++/CLI | Source Generator / fluent builder | Source Generator 能做接近 SwiftUI 的体验 |
| **Python** | cffi / pybind11 | context manager + builder | 用 `with vstack():` 表达层级 |
| **TypeScript** | WASM 编译 core | JSX / 链式调用 | WASM 让它天然跨平台 |
| **Swift** | module map | result builder（真 SwiftUI 语法） | macOS 二期 |
| **Kotlin** | JNI | DSL receiver lambda | Android/桌面 |

---

## 6. 声明式 DSL 的统一问题（**本方案最大的开放问题**）

SwiftUI 的优雅来自 Swift 的 `@ViewBuilder` + 泛型 + 值语义。**这套东西无法被一个统一 DSL 复现**，因为每个宿主语言的语言特性不同。

三条路线：

| 路线 | 做法 | 优点 | 缺点 |
|-----|------|-----|-----|
| **A. Fluent Builder 统一** | 所有语言都用 `.padding(8).background(c).onClick(f)` | 一套设计，实现简单，跨语言一致 | 层级结构难读，无类型安全的组合约束 |
| **B. 每语言原生 DSL** | 各语言写各自最地道的写法（macro / source-gen / result builder） | 体验最好 | N 份实现，N 份维护，行为难对齐 |
| **C. 混合（推荐）** | core 提供 builder 作为基座；每个语言写一层薄的原生 DSL 包住它 | 平衡 | 需要一套"DSL 语义一致性"测试 |

**推荐 C。** 并且定义一份 **DSL 语义规范**（不是语法规范），规定每种构造在各语言里必须产生等价的 VNode 树，用同一套 golden test 验证。

```rust
// Rust
view! {
    VStack(spacing: 8) {
        Text("Hello").font_size(24)
        Button("Click").on_click(|| count += 1)
    }
    .padding(16)
}

// Python
with vstack(spacing=8).padding(16):
    text("Hello").font_size(24)
    button("Click").on_click(lambda: state.set(count + 1))

// C#
VStack(spacing: 8, padding: 16) [
    Text("Hello").FontSize(24),
    Button("Click").OnClick(() => count++)
]
```
三者产出**完全相同**的 VNode 树 —— 这是可验证的目标。

---

## 7. 线程模型

```
┌─ UI Thread ────────────────┐   ┌─ Render Thread ─────────────┐
│ 输入事件分发                │   │ 消费不可变帧快照              │
│ VNode 重建 + diff          │──>│ 生成 command buffer          │
│ Measure / Arrange          │   │ 提交 GPU                     │
│ 构建不可变 FrameSnapshot    │   │ Present                      │
└────────────────────────────┘   └─────────────────────────────┘
                 └──── 双缓冲 / 三缓冲，无锁交接 ────┘
```
- 两个线程各有独立的 GPU 资源池（或用一个带 fence 的共享池）
- 动画时钟在 render thread：属性插值不经过 UI thread，避免 UI 线程繁忙时动画卡顿
- 宿主语言回调**只在 UI thread 上调用**，避免宿主侧加锁

---

## 8. 仓库结构建议

```
tacui/
├─ docs/                      # 本目录
├─ idl/                       # 接口定义（生成绑定的源）
├─ core/                      # C++ 核心（无平台依赖的部分）
│  ├─ base/                   #   句柄、代际索引、arena、字符串、几何
│  ├─ framework/              #   Element/RenderObject 树、diff、state、控件
│  ├─ scene/                  #   Layer 树、DrawBatch、脏标记
│  ├─ renderer/               #   批处理、AA、glyph atlas、路径
│  └─ text/                   #   shaping、布局、缓存
├─ rhi/                       # 渲染后端抽象
│  ├─ include/
│  ├─ d3d12/
│  ├─ vulkan/
│  └─ metal/
├─ platform/                  # 窗口、输入、IME、DPI、无障碍
│  ├─ windows/
│  ├─ linux/
│  └─ macos/
├─ capi/                      # C ABI 边界层（唯一导出符号）
├─ bindings/
│  ├─ rust/  csharp/  python/  swift/  kotlin/  ts/
├─ tools/
│  ├─ idlgen/                 # IDL -> 各语言代码生成器
│  └─ inspector/              # 调试器：视觉树 / 帧分析 / overdraw
├─ tests/
│  ├─ golden/                 # 截图回归测试
│  └─ dsl_parity/             # 跨语言 DSL 语义一致性测试
└─ examples/
```

**`tools/inspector` 要尽早做**（学 GacUI 的 Inspector / WPF 的 Live Visual Tree）。一个 UI 库没有可视化调试器，开发效率会低一个数量级。

---

## 9. 里程碑路线图

| 阶段 | 内容 | 预估（1-2 人） | 验收标准 |
|-----|------|--------------|---------|
| **M0 原型** | 单语言（C++）、D3D12、只画圆角矩形 + 文本、硬编码树 | 1-2 月 | 60fps 显示一个静态界面 |
| **M1 渲染内核** | RHI 抽象、batcher、analytic AA、glyph atlas、帧快照 | 3-4 月 | 复杂界面稳定 60fps，无闪烁 |
| **M2 Framework** | Element 树、diff、state 槽、Measure/Arrange、10 个基础控件 | 3-4 月 | 能写一个可交互的 Demo |
| **M3 C ABI** | capi 层、句柄、回调表、错误处理、版本协商 | 1-2 月 | C 程序能跑起完整界面 |
| **M4 绑定 + DSL** | Rust 绑定 + `view!` 宏；golden test 基建 | 2-3 月 | Rust 侧写出地道 DSL，通过 parity test |
| **M5 控件库** | 30+ 控件、样式/主题、虚拟化列表、动画系统 | 6-12 月 | 能替代现有桌面 App 的 UI |
| **M6 多语言** | C# / Python / TS 绑定 + 各自 DSL | 每语言 1-2 月 | 全部通过 parity test |
| **M7 硬骨头** | 文本编辑器控件、IME、无障碍、多窗口/弹窗 | 6-12+ 月 | —— |

**累计：即使全力投入，做到"可替代 WPF/Qt 的水平"也是 2-4 年的事。**

---

## 10. 风险与开放问题清单

### 🔴 高风险（会决定项目成败）

1. **范围（Scope）是最大的敌人。**
   GacUI 一个人做了十几年，至今没有主流采用。WPF 是微软几十人年的产物。一个 UI 库的 80% 工作量在**控件库 + 文本 + 平台集成**，不在渲染。渲染内核你可能 4 个月就做出来了，然后发现表格控件、文本编辑器、IME、无障碍还没开始。
   → 建议：先明确"我只做嵌入式 UI / 只做游戏工具链 UI / 只做特定垂直领域"，不要试图做通用桌面 UI 库。

2. **CPU 才是瓶颈，GPU 加速的收益被高估。**
   典型 UI 帧的分布：布局 20%、文本测量 30%、渲染命令生成 25%、GPU 光栅化 25%。GPU 加速只优化最后那 25%，而前三项是 CPU 密集。
   → 建议：把精力放在布局增量化和文本缓存上，GPU 只是让"可承受的绘制量"变大。

3. **"声明式 + Retained + 跨语言"三者的组合会互相拖累。**
   声明式要求频繁重建树 → 跨语言重建树的 FFI 开销大 → 为了省开销想 retained 复用 → retained 复用又让 diff 复杂。这是一个真实的架构张力，需要早期用真实 benchmark 验证（每帧重建 1000 节点的 FFI 成本到底是多少）。

4. **跨语言 DSL 无法真正统一。**
   SwiftUI 的体验依赖 Swift 的语言特性。你最终会得到 5 套"各自还行但彼此不同"的 DSL。这会稀释文档、示例、社区答案的价值。这是架构上的固有代价，不是能靠工程解决的。

### 🟡 中等风险（有明确技术方案但工作量大）

5. **Analytic AA 实现复杂。** 曲线细分、凹/凸处理、退化情况（自交、极短边）。第一版建议只做圆角矩形 + 文本，绕开它。
6. **中文字形 + 字体回退。** CJK 字表大、字体回退链复杂、竖排、变体字。HarfBuzz + ICU 只是起点。
7. **IME（输入法）。** 每个平台一套完全不同的 API，且与文本编辑器的光标/选区逻辑强耦合。这块经常被低估 3-5 倍工期。
8. **弹窗溢出到窗口外。** 需要一个独立的穿透/分层窗口（Windows 上 `WS_EX_LAYERED` 或 DirectComposition）、跨窗口的输入路由、失焦关闭。多窗口是 retained 框架的老大难。
9. **无障碍（Accessibility）。** 必须把语义树映射到 UIA / AT-SPI / NSAccessibility，且要支持屏幕阅读器的查询与事件。低优先级但不能不做（合规要求）。
10. **文字编辑器控件。** 这是所有 UI 框架最复杂的单个控件（选区、双向文本、光标移动规则、行内对象、撤销栈、虚拟化）。别低估。
11. **热重载 / 调试体验。** 声明式框架的用户会期待状态保留的热重载。这在 C++ core + 多语言宿主下非常难做。

### 🟢 已知但可控的问题

12. **是否需要反射/属性系统？**
    WPF 的 DependencyProperty 系统是它最复杂也最被诟病的部分。**本方案不需要它** —— 因为我们不做 XAML 数据绑定，state 由宿主语言管理，属性是简单的 `setProp(id, value)`。这是一个重大的简化，要守住这条线。
    ⚠️ 但要注意：一旦有人要求"从字符串按名字设属性"（主题、动画目标属性、序列化），你就需要轻量反射。建议预留一个 `PropertyId` 枚举 + 类型擦除的值容器，而不是完整 RTTI。

13. **Core 用什么语言？**
    你选了 C++。可以，但要意识到：
    - C++ 是最难写声明式 DSL 的宿主语言（没有好用的宏）
    - 多语言绑定的生态工具（如 uniffi）主要面向 Rust
    - Rust 在内存安全、绑定生成、WASM 编译上有明显优势
    → 如果 C++ 不是硬性要求，值得重新评估。若是硬性要求，C++ 仅作 core，DSL 由各宿主语言负责，这个组合是合理的。

14. **构建系统的复杂度。** 7 种语言的绑定 × 3 个平台 × 多种构建系统（CMake / Cargo / MSBuild / setuptools / Gradle）。CI 会成为长期负担。
    → 建议：绑定层做成独立仓库或独立 workspace，不要和 core 混在一起构建。

15. **许可证与字体。** 内置字体、图标、以及静态链接 HarfBuzz/ICU/FreeType 的许可证合规。

16. **测试策略。** UI 库必须做**截图回归测试**（golden image），且要处理 GPU 差异带来的像素抖动（用容差比较）。这套基建要在 M1 就搭好。

### ❓ 需要你现在拍板的问题

| # | 问题 | 影响 |
|---|------|------|
| ~~Q1~~ | ~~目标场景~~ | ✅ **已定：桌面 App + 游戏工具链**（见第 12 节） |
| Q2 | Core 语言：C++ / Rust？ | ✅ 见第 11 节 trade-off，推荐 **C++** |
| Q3 | 第一版平台：只 Windows，还是 Win + Linux？ | 影响 RHI 和平台层工作量（约 1.5x） |
| Q4 | DSL 路线选 A / B / C？（推荐 C） | 影响长期维护成本 |
| Q5 | 是否接受"第一版不支持任意矢量路径"？ | 能省 3-4 个月，但**游戏工具链需要它**（节点图连线）→ 必须提前到 M1 |
| Q6 | 团队规模与时间预算？ | 决定里程碑要不要砍 |
| Q7 | 扩展模型：组合优先还是继承优先？（见第 11.3 节） | **决定语言选型的真正前提** |

---

## 11. Core 语言选型 Trade-off

### 11.1 结论先行

> **推荐 C++ core**，因为「游戏工具链」这个目标场景引入了两个 C++ 的硬优势（原生 GPU 互操作、自定义绘制控件的 core 级扩展）。
> 但如果你能接受「扩展模型完全组合化 + 不追求引擎交换链直连」，**Rust core 在绑定和多语言工程上领先 6-12 个月的工作量**。
>
> **真正的决策变量不是语言本身，是第 11.3 节的扩展模型。**

### 11.2 逐维度对比

| 维度 | C++ core | Rust core | 权重（本项目） |
|-----|---------|----------|--------------|
| **多语言绑定生态** | 只有 cbindgen + 手写 IDL。Swig 对现代 C++ 支持差。**6 语言绑定 ≈ 2-4 人月** + 长期维护 | `diplomat`（Google，从 Rust 生成 C++/Swift/Kotlin/JS/Java）· `uniffi`（Mozilla）· `cbindgen` + `bindgen` + `wasm-bindgen`。**同等规模 ≈ 0.5-1 人月** | 🔴 **极高** |
| **声明式 DSL（core 语言自身）** | 没有可用宏。只能做 fluent builder，体验平庸 | `proc-macro` 是除 Swift 外最好的声明式 DSL 载体，`view!{}` 可以直接对标 SwiftUI | 🟡 中（其他宿主语言的 DSL 不受影响） |
| **GPU / RHI 直连** | D3D12 / Vulkan / Metal 一等公民，零绑定层。可与引擎交换链、共享纹理、DirectComposition 直接互操作 | 只能用 `wgpu`（WebGPU 抽象，会挡住部分低层能力）或 `ash`/`windows-rs` 写大量 unsafe。与引擎交换链互操作要绕过抽象层 | 🔴 **极高**（游戏工具链） |
| **保留树 / 生命周期安全** | UAF 和 data race 是这个项目最可能的崩溃来源（回调密集 + 跨帧存活 + 多线程） | 借用检查器在带父指针的树上会**强烈对抗你**——这是 Rust GUI 框架的经典痛点。但解法（arena + 代际索引）**本来就是你为了 C ABI 必须做的设计**，实际阻抗比通常低 | 🟡 中偏上 |
| **文本 / 图形生态** | HarfBuzz · ICU · FreeType · Skia 直接可用，参考实现最多 | `swash` / `rustybuzz` / `fontations` / `cosmic-text` 已相当成熟。**Vello（现代 compute 2D 渲染器）是 Rust**，是这个领域最好的开源参考 | ⚪ 平 |
| **构建 / CI / 分发** | CMake + vcpkg/conan，MSVC/clang/gcc 矩阵，`/MT` vs `/MD`、异常/RTTI 开关的 ABI 地雷 | Cargo 一站式，交叉编译、可复现构建、静态库都很干净 | 🟡 中（能靠投入缓解） |
| **受众 / 贡献者池** | 游戏工业界全是 C++ 开发者。工具链团队能直接读改 core | 池子小得多，但在快速增长 | 🔴 **高**（游戏工具链） |
| **引擎嵌入** | Unreal / 自研引擎可**直接链接**，用引擎的分配器/日志/任务系统很自然 | 需编译成 staticlib 走 C ABI 链接进引擎，可行但不常见 | 🔴 **高**（游戏工具链） |
| **迭代速度 / 工具链** | 编译慢，工具链要自己搭（clangd / ASan / clang-tidy） | 编译也慢，但 `cargo test` / `clippy` / `miri` 开箱即用 | ⚪ 平，Rust 略优 |
| **自定义控件扩展** | 用户可用 core 语言子类化 / 覆写 `Measure` `Paint`。**这是节点图、时间轴、dock 布局的必需品** | 用户必须用 Rust 写 core 级控件。C++ 团队接入成本高 | 🔴 **极高**（见 11.3） |

### 11.3 ⭐ 决定性的问题：扩展模型

**这才是真正决定语言的变量，而不是「哪个语言更现代」。**

```
问题：用户要做一个节点图编辑器，需要自定义绘制 + 自定义命中测试 + 自定义布局。
      这些东西怎么实现？

路线 A · 继承优先（WPF / Qt 式）
   用户在 core 语言里写 class NodeGraph : public Control { ... }
   -> core 语言必须是团队能写的语言
   -> 游戏工具链团队 = C++ 团队
   -> 只能是 C++ core

路线 B · 组合 + 回调优先（Flutter / SwiftUI 式）
   用户通过 DSL 组合已有控件，自定义绘制通过注册 paint 回调
   -> core 语言对用户不可见，可以自由选
   -> Rust core 的绑定/安全优势全部兑现
   -> 但「自定义绘制回调」跨 FFI 每帧调用，性能要仔细设计
      （应该是：回调返回一个 op 列表缓冲，而不是逐图元 FFI）
```

**推荐：组合优先 + 通过 C ABI 暴露 paint/layout 回调（路线 B），但保留 C++ 直接子类化的能力（路线 A）。**

这样：
- 80% 的用户场景走组合，跨语言一致
- 复杂控件的作者如果是 C++ 团队，直接写 C++ 子类，性能最好
- 这就**要求 core 是 C++** —— 否则路线 A 对目标受众不可用

### 11.4 最终建议

**选 C++ core。** 理由按重要性排序：

1. **游戏工具链需要自定义绘制控件**，而目标受众是 C++ 团队。这是路线 A 的硬需求，直接锁死。
2. **引擎嵌入和交换链互操作**在 C++ 下是零成本，在 Rust 下要绕过 `wgpu` 抽象层。
3. Rust 的绑定优势（约省 2-3 人月）**可以通过一次性投入 IDL + codegen 补回来**，而且你为了另外 5 个语言本来就得建这套东西。
4. 「游戏工业界全是 C++」意味着贡献者池和采纳门槛都在 C++ 这边。

**你要接受的代价（明确写下来，别自欺）：**
- ❌ C++ 侧用户拿不到 `view!{}` 级别的 DSL，只有 fluent builder
- ❌ 崩溃风险更高 → **必须** 用 arena + 代际索引 + CI 里跑 ASan/TSan，这不是可选项
- ❌ 绑定层要多写 2-3 人月，且每次改 API 要手动同步
- ❌ 构建系统复杂度显著更高

**什么时候应该改选 Rust：** 如果你确认「用户永远不会写 core 代码，扩展全部走 DSL + C ABI 回调」且「引擎嵌入只走纹理/图片输出，不直接共享交换链」——那 Rust 全面胜出。

**对冲手段（不管选哪个都建议做）：**
- **M0 原型用两种语言各写一遍**（窗口 + 清屏 + 一个圆角矩形 + 一行文本，各约 1-2 周）。这是唯一能真正终结争论的办法，成本很低。
- **C ABI 从 M0 就严格设计**：这是唯一的保险。ABI 对了，理论上 core 可以换；实践中没人换过，但严格的 ABI 边界本身就值这个钱。

---

## 12. 目标场景范围（已定：桌面 App + 游戏工具链）

这个组合比「通用桌面 UI 库」聚焦，但两者的并集仍然很大。

### 并集：都要做的
- 完整控件库（30-50 个）
- **文本编辑器控件**（属性面板的数值输入、游戏工具里的脚本/配置编辑器）→ 不可砍
- 多窗口 / 弹窗 / 工具提示（溢出窗口外）
- 高 DPI、多显示器
- 主题 / 深色模式
- 虚拟化列表 / 树 / 表格

### 游戏工具链**独有**的高优先级项
| 能力 | 说明 | 对架构的影响 |
|-----|------|------------|
| **Docking 布局** | 面板拖拽、停靠、浮动（对标 Visual Studio / ImGui docking） | 需要可序列化的布局状态 + 浮动面板窗口 |
| **节点图 / 曲线编辑器** | 自定义绘制 + 命中测试 + 缩放平移 | 🔴 **必须支持任意矢量路径**（连线、贝塞尔），AA 方案必须提前到 M1 |
| **引擎交换链嵌入** | UI 渲染进引擎的渲染目标，而不是独立窗口 | RHI 必须支持「渲染到外部纹理」+ 外部同步原语 |
| **性能可视化** | 帧时间图、GPU 计数器 | 内建 profiler 变成必需品 |
| **极高帧率** | 工具可能跑在 120/144Hz 显示器上，且与引擎共享 GPU | 动画时钟不能假设 60Hz |

### 桌面 App**独有**的高优先级项
| 能力 | 说明 |
|-----|------|
| **IME** | 中文/日文输入，与文本编辑器强耦合。**游戏工具的文本框也需要**，所以是并集 |
| **无障碍** | UIA。桌面 App 需要，工具类可降级 |
| **系统集成** | 文件对话框、拖放、剪贴板、托盘、任务栏、窗口贴靠 |

### 范围结论
- **Q5 的答案变了**：原本建议「第一版不做任意矢量路径」以省 3-4 个月，但**节点图是游戏工具链的核心场景**，所以解析 AA 必须从 M1 就开始做。这是本决策带来的主要工期变化（约 +3 个月）。
- **建议的第一个里程碑 Demo 就是节点图**，而不是「Hello World 按钮」。它一次性压测了自定义绘制、路径 AA、命中测试、缩放平移、dock 集成——是整个架构最强的验收标准。

---

## 13. 架构范式 Trade-off：WPF 式 vs Flutter 式

### 13.0 先破除一个误解

> **「Flutter 式每次重建」和「retained mode」不冲突。**

这是最常见的混淆，也是你问这个问题的原因。两者在**不同的层**上：

```
Flutter 的"重建"重建的是 描述（Widget/VNode）——一次廉价的纯值构造
WPF 的"保留"   保留的是   实体（Element / RenderObject / GPU 资源）
```

Widget 重建 ≈ React 重建 virtual DOM。真正的保留状态在它下面，完全不受影响。
**retention 程度是独立的一个轴**，和树模型无关：Flutter 式和 WPF 式都可以做到深度 retained。

而且要说清楚：**WPF 本身并没有你想要的那么 retained**——它的视觉树是保留的，但**光栅化是每次 damage 重做的**（重绘 display list）。
本文 §3 提的「GPU 资源常驻 + 每帧只生成 command buffer」其实**比 WPF 更 retained**。所以「因为想要 retained 所以选 WPF 式」这个推理是不成立的。

### 13.1 两者的本质差别

| | WPF 式 | Flutter 式 |
|---|--------|-----------|
| **一个节点是什么** | 一个**可变对象**，同时是声明 + 状态 + 布局 + 渲染 | **三棵树**：Widget(不可变描述) / Element(身份+状态) / RenderObject(布局+渲染) |
| **变更方式** | 原地修改 + 失效传播（`InvalidateMeasure`） | 重建描述 + diff + 应用差异 |
| **属性系统** | DependencyProperty：全局注册、优先级链、值继承、强制转换、校验、可动画化 | 无属性系统。属性就是构造参数 |
| **复用机制** | 继承 `Control`，覆写 `MeasureOverride` / `OnRender` | 组合。`Container` = 一堆 Widget 包起来 |
| **数据绑定** | `{Binding}` + 反射 + `INotifyPropertyChanged`，一等公民 | 无绑定语言。「绑定」= 重建 |
| **样式/触发器** | Style / Trigger / Template 可以改任意控件的任意属性 | 无。Theme 由控件自己主动读取 |

### 13.2 逐维度对比

| 维度 | WPF 式 | Flutter 式 | 对 TacUI 的权重 |
|-----|--------|-----------|---------------|
| **需要反射吗** | ✅ **需要**。DP 是字符串键 + 反射驱动（`Register("Width")`、`Path=Foo.Bar`）。等价于 GacUI 的 `VlppReflection` | ❌ **不需要**。Widget 就是带类型构造参数的普通 struct | 🔴 **极高** |
| **变更粒度** | 细粒度。改一个属性 O(1)，只失效受影响子树 | 粗粒度。重建子树 + diff。有恒定开销 | 🟡 中 |
| **C++ 分配成本** | 低。原地改，几乎不分配 | ⚠️ **高**。Dart 重建便宜是因为分配快 + GC 免费。**C++ 重建 = malloc/free churn** | 🔴 **极高（C++ 特有）** |
| **跨 FFI 边界** | 每次改属性一次调用 → **chatty，但只有变更时发生**。绑定简单（一堆 setter） | **统一、可批处理**（整棵子树序列化成一个 buffer 一次提交），但**必须配 diff 引擎** | 🔴 极高 |
| **热重载** | ❌ 很难。没有 diff 机制，为了热重载得**重新发明 diff** | ✅ 几乎免费。Element 树持状态 + 新代码重建 Widget + reconcile，这就是 Flutter 热重载的全部原理 | 🔴 高（游戏工具要） |
| **大量节点扩展性** | ✅ 好。细粒度失效天然避免根重建 | ⚠️ 差。10 万节点改一个 state 会重建整棵子树 → 需要 `const`/`RepaintBoundary`/`AnimatedBuilder` 一整套优化生态 | 🟡 中 |
| **调试可预测性** | ❌ 差。「这个属性为什么是这个值」是 WPF 第一大难题（优先级链 + 绑定错误 + 继承值） | ✅ 好。「树是这样，所以渲染是这样」，单向数据流 | 🔴 高（新库要靠可预测性建立生态） |
| **组合 vs 继承** | 继承。树浅、性能好、自定义控件自然 | 组合。树深（一个 `Container` = 5+ 节点）、但跨语言统一、可测试 | 🔴 极高（见 §11.3） |
| **它们 → 非 core 语言** | ❌ 必须能写 core 语言才能扩展。直接封死 C#/Python 用户 | ✅ 组合对任何语言都一样 | 🔴 极高 |
| **主题/样式能力** | ✅ 强。Trigger 可以不写代码改任意控件的任意属性 | ❌ 弱。Theme 是约定，控件要主动读 | 🟡 中（游戏工具要换肤） |
| **概念数量** | 多（DP + 绑定 + 样式 + 触发器 + 模板 + 优先级） | 少（三棵树 + reconcile） | 🟡 中 |
| **行业趋势** | 1990s-2000s 设计。WinUI3 仍在用，是现在的异类 | SwiftUI / Compose / React / Flutter 全部收敛到「不可变描述 + diff + 稳定身份树」 | 🟡 参考 |
| **布局协议** | `Measure(availableSize) -> desiredSize`。自由度过大，可以返回无意义的值 | `BoxConstraints(min/max w/h, tight/loose/unbounded) -> Size`。**更严格，且是纯值类型** | 🟡 中（值类型对 C ABI 友好得多） |

### 13.3 ⭐ 对 TacUI 最关键的三条

**① 反射：WPF 式的最大隐藏成本**

DP 系统在 C++ 里意味着你必须造一套反射（`DependencyProperty::Register("Width", typeid(...), metadata)` + 按字符串/路径取值）。
这正是 GacUI 花了大力气做 `VlppReflection` 的原因——**GacUI 有 XML 资源和 Workflow 脚本，反射是它的立身之本**。

但你**明确不要 XML**。那么 DP 系统 70% 的价值就失去载体了：
- ❌ XAML 里的 `{Binding Path=Foo.Bar}` → 没有
- ❌ XAML 里的 `<Trigger>` / `<Style>` → 没有
- ❌ XAML 里 `TargetProperty="Width"` 的字符串寻址 → 没有
- ✅ 剩下：值继承、强制转换、动画覆盖本地值

**你要为一个不需要反射的架构，付一整套反射系统的代价。**

**② C++ 的分配成本：Flutter 式在这里的真实代价**

Flutter 在 Dart 里敢每帧重建整棵树，是因为 Dart 分配极快、GC 让释放免费。
C++ 里「重建描述树」= 一堆小对象 malloc/free，这是**真实的、每个变更都要付的成本**。

必须的缓解手段（不是优化，是架构前提）：
- VNode 用 **arena allocator + 每帧/每次 reconcile 后整体 reset**
- 小集合用内联存储（`SmallVector`）
- 复用对象池，避免 malloc 抖动
- **只在 state 变化时重建，不每帧重建**（动画走直写通道，见 §2.3）

**③ 跨 FFI：两者其实打平，但打法不同**

| | WPF 式 | Flutter 式 |
|---|--------|-----------|
| 边界流量 | 每次属性变更一次调用。频繁但只在变更时 | 一次提交整棵子树。**可批处理**（序列化成一个 buffer），但必须有 diff |
| 绑定工作量 | 低。就是一堆 setter，适合 IDL 自动生成 | 高。要设计紧凑的编码格式 + diff 引擎 |
| 可批处理性 | ⚠️ 差。每个 mutation 有语义和顺序依赖，理论上可以记成 mutation log 再批量提交——**但那样你就重新发明了命令缓冲，而且失去了 reconcile 能力** | ✅ 好。天然是「一次提交一棵树」的批量协议 |

结论：**WPF 式简单但 chatty，Flutter 式复杂但可批处理。** 对多语言绑定来说，Flutter 式的统一协议更好自动化。

### 13.4 WPF 式真正值钱的部分（该拿走的）

不要因为 DP 系统不划算就否定整个 WPF 模型。这几样是真金：

1. **属性值继承（Value Inheritance）** —— 在根上设 `FontSize`，整棵树继承。游戏工具的 dock 布局特别需要「一处换肤、全局生效」。Flutter 的 `InheritedWidget` 是这个的笨重版本。
   → **拿走**，但实现成轻量的 `InheritedProperty` 通道，不是 DP。
2. **动画覆盖本地值** —— 动画可以驱动任何属性而不修改元素本身，动画结束自动回落。
   → **拿走**，实现成 §2.3 的直写通道 + 明确的优先级（animation > local）。
3. **细粒度失效** —— 改一个属性只失效受影响子树，不重建。
   → **拿走**，作为 Flutter 式 diff 之外的**第二条快路径**。
4. **面向命令式调用的可变 API** —— 游戏编辑器的 C++ 模型层（场景图、资源库）经常在外部线程/外部代码里直接改数据，希望能直接推给 UI。
   → **拿走**，作为直写通道的公开 API。

**不拿走的：** DependencyProperty 系统本身、字符串键属性寻址、反射。

### 13.5 建议：Flutter 骨架 + WPF 直写快路径，明确拒绝 DP

```
┌─────────────────────────────────────────────────────┐
│ 宿主语言 DSL  →  VNode (不可变、arena 分配)           │  ← Flutter
│                    ↓ reconcile (按 key / 位置)        │  ← Flutter
│              Element 树 (身份 + state 槽)             │  ← Flutter
│                    ↓                                  │
│              RenderObject 树 (Measure/Arrange/Paint)  │  ← Flutter + WPF 的布局
│                    ↓                                  │
│              Layer / DrawBatch (GPU 资源常驻)          │  ← WPF/Win2D 的 retained
└─────────────────────────────────────────────────────┘
         ↑                              ↑
    使用方 A：声明式                 使用方 B：命令式 / 动画
    改 state → 重建 → diff           直写 RenderObject 属性
    （走完整链路，可预测）            （绕过 diff，O(1)，高频）
```

**两条路径并存**，这就是 Flutter 内部本来就有的结构（`setState` vs `RenderObject.markNeedsPaint` / `AnimatedBuilder`），只是我们要把它做成一等的公开 API，因为游戏工具链需要路径 B。

**收益：**
- 不需要反射 → 省掉一个 GacUI `VlppReflection` 级别的子系统
- 静态类型 → C++ / C ABI 友好
- 组合优先 → 所有宿主语言体验一致（解决 §11.3 的路线 A/B 矛盾）
- 热重载可行（虽然 C++ core + 多语言宿主下仍然很难，但至少架构支持）
- 直写快路径 → 动画和命令式驱动不付重建成本
- 值继承 + 主题从直写通道实现

**代价（明确接受）：**
- 要写一个健壮的 diff/reconcile 引擎（含 key、列表重排、异步 reconcile）≈ **2-3 人月**，且是长期 bug 来源
- Element 树额外内存（每个 VNode 对应一个 Element）
- 组合模型导致树更深，布局遍历节点数增多
- 「什么时候重建、为什么重建」的性能模型对使用者是非显然的，要写文档、做工具
- C++ 下必须配套 arena / 对象池，否则重建开销吃掉收益

### 13.6 什么时候该反过来选 WPF 式

诚实地说，如果满足以下**全部**条件，WPF 式（纯原地修改，不做 diff）是更务实的选择：

1. 你能接受**放弃热重载**
2. 你的扩展模型是**继承优先**（§11.3 路线 A），用户都是 C++ 团队
3. 你**不追求跨语言体验一致**——只把非 core 语言当「遥控器」，不做地道 DSL
4. 界面规模巨大（>10 万节点），细粒度失效的扩展性优势是刚需
5. 你需要非常强的 Style/Trigger 能力，且愿意为此造反射系统

**但这 5 条里，2 和 3 与「多语言绑定 + 各语言地道 DSL」的核心目标直接冲突。**
所以除非你放弃 G4/G5 两个目标，否则还是 Flutter 式骨架更匹配。

### 13.7 不可逆性 & 对冲

| 选择 | 反悔成本 |
|-----|---------|
| Flutter 式 → 补 WPF 式直写通道 | **低**。直写通道本来就是 Flutter 内部有的东西，加公开 API 即可 |
| WPF 式 → 补 diff/reconcile | **高**。diff 需要 Element 树承载身份和 state，而 WPF 式的可变对象模型没有这个位置，等于重做 |

**结论：Flutter 式骨架是低风险的默认选择，因为它包含 WPF 式的快路径；反过来不成立。**

**唯一的对冲手段：第一天就建 Element 树（身份 + state 槽）。**
只要 Element 树在，上面两条路径都能长出来。Element 树是**最难 retrofit 的那一层**——这是本决策里唯一真正不可逆的部分。

### 13.8 对本项目的最终建议

- **选 Flutter 式三棵树作为骨架**，因为：无反射、跨语言一致、组合优先能解开 §11.3 的死结、可热重载、行业已验证
- **同时把 WPF 的直写快路径做成一等 API**（动画 + 命令式驱动 + 值继承/主题）
- **明确不做 DependencyProperty 系统**——它在无 XML 的前提下性价比极低
- **布局协议采用 Flutter 的 `BoxConstraints` 值类型**，而不是 WPF 的自由 `MeasureOverride`
- **第一条铁律：先写 Element 树。** 它是唯一不可逆的架构决定。

### 13.9 「哪个更优秀」——直接结论

> **架构设计上 Flutter 式明显更优秀，且不是「更新所以更好」。WPF 式在三个具体轴上确实更强。**

**Flutter 更优秀的实质性理由（按重要性）：**

1. **单向数据流 vs 组合式可变状态。** WPF 允许任何人在任何时刻从任何来源改任意属性（代码 / XAML / Style / Trigger / Binding / Animation / 继承值），最终值由**优先级链**决定。这是组合爆炸，也是 WPF 第一大调试难题的根源。Flutter 里 UI 是 state 的纯函数，只有一个方向。
2. **失败模式的成本。** WPF 失败得**安静且困惑**（值不对、无报错、要爬优先级链）；Flutter 失败得**响亮且局部**（重建开销可见、树可检查）。对一个没有 Stack Overflow 语料的新库，安静困惑的失败模式代价高一个数量级。
3. **可测试性。** Flutter widget 可脱离运行时独立测试；WPF 要跑 Dispatcher、要 STA、要真实视觉树。
4. **身份与状态显式化。** WPF 把「对象」和「状态」揉在一起，所以热重载、diff、时间旅行调试都做不了。Flutter 把描述和身份分开，这三样都变成可能。
5. **组合 > 继承。** 这是被反复验证的教训（React class → hooks，WPF → Compose，Qt Widgets → QML）。
6. **不需要反射。** 静态类型、编译期检查，而不是运行期字符串路径失败。

**额外一条，对本项目特别重要：**
> WPF 的模型要求框架作者**一开始就把所有事情设计对**——DP 元数据、优先级规则、强制转换规则都是烧死的，后期改会破坏所有使用者。
> Flutter 的模型在边缘上可扩展性强得多，因为 Widget 就是代码。
> **一个全新的库注定会在早期做错很多事，所以「更可塑的模型」在这里价值尤其高。**

**WPF 确实更优秀的三个轴（诚实）：**
- **细粒度更新 / 超大规模树**：DP 失效是 O(change)，Flutter diff 是 O(子树)。10 万节点场景 WPF 模型更稳。
- **样式表达力**：Trigger 能不改源码重绘任意第三方控件。Flutter 做不到（Theme 只是约定，控件必须主动读）。
- **命令式 / 外部数据源亲和性**：WPF 直接绑可变 .NET 模型（`INotifyPropertyChanged`），对数据密集型桌面 App 很自然。
- 以及**成熟度**：20 年的边界情况都被趟过。这条不该被轻视。

**关键判断框架：**
> WPF 的模型是 **2006 年约束下的产物**——.NET、XML 设计师工具、反射随手可得、单一语言、桌面为王。
> **这些约束对 TacUI 一个都不成立。** 照搬 WPF = 继承了一堆针对你并不存在的问题的解药。
> 连微软自己都没往前推这个模型：WinUI3 保留 DP 主要是为了兼容，不是因为它更好。

**⚠️ 区分两件事：**
- **Flutter 的架构模型**优秀 ✅
- **Flutter 这个框架**处处更好 ❌ —— 它有独立的问题：二进制体积、不用原生控件、无障碍偏弱、文本渲染与平台不一致、深 Widget 树、重建性能模型非显然

你在选的是**模型**，不是要不要用 Flutter。本文建议的是拿走 Flutter 的树模型 + WPF 的直写快路径，而不是照抄任何一个框架。

---

## 14. 具体设计：Flutter 树模型 + WPF 直写快路径

### 14.0 这个设计不是我们发明的 —— Flutter 自己就这么干

Flutter 内部**已经同时存在这两条路径**，只是没有把它做成一等公开 API：

| Flutter 构件 | 走哪条路 |
|-------------|---------|
| `setState` / `AnimatedBuilder` | 重建 + diff（慢路径） |
| `ScrollPosition` / `ViewportOffset` | **直写**：滚动时直接写 RenderObject 的 offset，**零重建** |
| `CustomPainter(repaint: listenable)` | **直写**：listenable 触发只标 paint dirty，**不重建 widget** |
| `RepaintBoundary` | 隔离重绘范围 |

滚动是 Flutter 里最高频的交互，它**没有走重建**，而是造了 `ViewportOffset` 这个「token 持有的覆盖值」通道——这正是我们要泛化成一等 API 的东西。

所以本设计的核心工作不是发明机制，而是**把 Flutter 内部的隐式快路径显式化，并开放给宿主语言**（游戏工具链需要它）。

### 14.1 完整分层

```
┌─ 宿主语言（Rust / C# / Python / ...）──────────────────────────┐
│  DSL 构造 VNode（arena 分配，一次提交整棵子树）                  │
└──────────────────────────┬────────────────────────────────────┘
                           │ commit(buffer)          ← 低频，事件驱动
┌─ UI Thread ──────────────▼────────────────────────────────────┐
│  reconcile → Element 树（稳定身份）                             │
│    ├─ state 槽（跨帧持久，按位置寻址）                           │
│    ├─ 回调表（宿主函数指针 + userdata）                          │
│    └─ → create / update / destroy RenderObject                 │
│                                                                │
│  RenderObject 树                                                │
│    ├─ base props   ← reconcile 写（低频）                       │
│    ├─ animSlot     → 指向 override 值（高频，每帧）              │
│    ├─ Measure / Arrange（只被 base 变化触发）                    │
│    └─ Paint → DrawBatch                                        │
└──────────────────────────┬────────────────────────────────────┘
                           │ 不可变帧快照（双缓冲 / 三缓冲）
┌─ Render Thread ──────────▼────────────────────────────────────┐
│  读 override 值 → 生成 command buffer → 提交 GPU               │
│  GPU 资源常驻（顶点缓冲 / glyph atlas / pipeline）跨帧不变        │
└───────────────────────────────────────────────────────────────┘
                    ▲
                    │ 直写通道（高频，绕过 UI Thread 的重建）
    ┌───────────────┴────────────────┐
    │ 宿主命令式代码 / 动画系统        │
    │  A. 声明式动画（core 内建时钟）   │
    │  B. 外部数据推送（游戏编辑器模型层）│
    └────────────────────────────────┘
```

### 14.2 核心数据结构

```cpp
// ── 身份层：Element 持状态，跨帧存活 ──────────────────────
struct Element {
    ElementId      id;           // 路径 + key 哈希，稳定
    TypeId         type;         // Widget 类型（决定能否复用）
    Key            key;          // 可选，列表重排用
    Element*       parent;
    SmallVector<Element*> children;

    StateSlot*     states;       // 按位置索引的 state 槽
    uint32_t       stateCount;

    RenderObject*  renderObject; // 可能为 null（组合型 Widget，见 14.6）
    OverrideToken  token;        // 本节点当前活跃的直写租约
};

// ── 渲染层：base 与 override 分离 ────────────────────────
struct RenderObject {
    Rect        bounds;          // 布局结果（只被 base 变化触发重算）

    PropsBase   base;            // ← reconcile 写，来自 VNode 属性
    uint32_t    animSlot;        // → override 值在 AnimBuffer 中的槽位

    DrawBatch*  batches;         // 结构变化才重建
    GPUResource* resources;      // 跨帧常驻
};

// ── 覆盖值：SoA，可被宿主直接写 ───────────────────────────
struct AnimBuffer {              // 结构体数组 → 数组结构体
    float* opacity;   uint32_t n;
    float* tx, *ty;                  // 平移（paint-only）
    float* sx, *sy;                  // 缩放（paint-only）
    float* rot;
    float* bgColor;                  // RGBA
    uint8_t* active;                 // 每个槽位是否被覆盖
    uint32_t generation;             // 代际，防悬垂
};

// ── 生效值：唯一的一条优先级规则 ──────────────────────────
inline float effectiveOpacity(const RenderObject& r, const AnimBuffer& b) {
    return b.active[r.animSlot] ? b.opacity[r.animSlot] : r.base.opacity;
}
```

**这就是我们想要的「DP 的那一小块」**：不是全局优先级链，而是**恰好一层覆盖 + 显式租约**。

### 14.3 唯一的优先级规则

```
生效值 = override(若 token 活跃) 否则 base
```

对比 WPF 的优先级链（local > animation > trigger > style > inherited > default）——我们只保留 **animation vs local** 这一对，而且 override 由**显式 token** 持有，生命周期确定。

| | WPF DP | 本设计 |
|---|--------|-------|
| 覆盖层数 | 6+ 级，全局规则 | **1 级，局部规则** |
| 谁设置 | 任何人、任何来源 | 只有持 token 的代码 |
| 何时失效 | 由优先级自然回落，隐式 | **token revoke，显式** |
| 需要反射 | ✅ | ❌（槽位是编译期分配的整数） |
| 调试 | 「为什么是这个值」极难 | 「override 还活着吗」一个布尔 |

### 14.4 reconcile 与直写如何共存

```cpp
void reconcile(Element* e, const VNode* v) {
    if (e->type != v->type || e->key != v->key) {
        unmount(e);                  // ← revoke token，释放 GPU 资源
        mount(e, v);                 // ← 重建子树
        return;
    }

    // 关键：只写 base，绝不碰 override
    bool layoutDirty = e->renderObject->updateBase(v->props);
    if (layoutDirty)  e->renderObject->markNeedsLayout();
    else if (v->propsChanged) e->renderObject->markNeedsPaint();

    reconcileChildren(e, v->children);
}
```

**规则：** `reconcile` 只写 `base`。`override` 的生命周期完全由 token 掌握。
所以「DSL 里写 `.translate(0)` + 动画覆盖 translate」不会打架 —— 下次 reconcile 更新 base，override 继续生效，直到动画 revoke。

### 14.5 使用方 API：NodeRef

宿主永远不直接碰 struct，只拿 `NodeRef`（代际句柄）。这样 v1 的简单实现可以无痛升级到 v2 的 SoA。

```rust
// ── 路径 A：声明式（改 state → 重建 → diff）────────────────
view! {
    VStack {
        Text(format!("count = {}", count.get()))
        Button("+1").on_click(move |_| count.set(count.get() + 1))
    }
}

// ── 路径 A′：声明式动画（语法糖，编译成路径 B + token）──────
view! {
    Rect()
        .size(100, 100)
        .translate(tx.animate(spring(0.0, 1.0)))   // 走直写，不重建
}

// ── 路径 B：命令式直写（游戏编辑器模型层）──────────────────
let node: NodeRef = scene.get_ref("marker_42");
node.write(|w| {                    // 一次 FFI，批量写
    w.translate(120.0, 88.0);
    w.opacity(0.6);
});                                 // w 离开作用域 → revoke

// ── 路径 B′：批量推送（每帧上万个数）───────────────────────
let buf = ui.anim_buffer();         // 拿到 SoA 裸指针区间
for m in markers {                  // 零 FFI
    buf.translate_x[m.slot] = m.x;
    buf.translate_y[m.slot] = m.y;
}
ui.commit_buffer(&[..dirty_range]); // 一次提交
```

### 14.6 NodeRef 指向谁？—— 「primary render object」

组合模型下一个 DSL 节点可能产生多个 Element（`Container` = 5+ 个 Widget），但只有一个（或零个）RenderObject。

| Widget 类型 | 有 Element | 有 RenderObject |
|------------|-----------|----------------|
| `RenderWidget`（`Padding` / `Rect` / `Text`） | ✅ | ✅ |
| `CompositionWidget`（`Container` / `Column`） | ✅ | ❌ |

`NodeRef.render()` 返回**最近的子孙 RenderObject**（Flutter 的 `Element.renderObject` 就是这个语义）。
约定：`NodeRef` 只对 `RenderWidget` 保证有效，对 `CompositionWidget` 会解析到它的第一个 RenderWidget 后代 —— 文档要写清楚，这是使用者最容易困惑的地方。

### 14.7 这个设计新引入的问题（诚实清单）

| # | 问题 | 缓解 |
|---|------|------|
| 1 | **「为什么我的 translate 没生效」** → override 还活着 | 规则只有一层；Inspector 里把 override 值**高亮显示**；`write()` 用作用域（RAII）保证 revoke |
| 2 | **直写绕过 diff → 不会自动清理** | token 必须有确定生命周期：作用域块 / 显式 handle。**不允许无主的直写** |
| 3 | **测试矩阵翻倍**：每个属性要测「只 base」「只 override」「两者共存」 | golden test 基建里加一个「override 组合」维度 |
| 4 | **NodeRef → RenderObject 非 1:1** | §14.6 的 primary 语义 + 文档 + 运行时警告 |
| 5 | **undo/redo 与状态持久化只覆盖 base**，override 是瞬态 | 这是**正确**语义（没人想 undo 动画中间帧），但要明确写进文档 |
| 6 | **宿主拿到裸指针可越界写** | 代际校验 + commit 时范围校验；v1 不暴露裸指针 |
| 7 | **哪些属性能直写？** | **v1 只允许 paint-only**（transform / opacity / color / paint 层 offset）。**布局相关属性必须走重建**，否则直写会打乱布局缓存 |
| 8 | **动画 tick 在哪个线程？** | v1：UI 线程（事件驱动，UI 线程大部分时间空闲，够用）。v2：渲染线程 + AnimBuffer 双缓冲 + fence。**API 形状要让 v2 是 drop-in** |

### 14.8 建议的落地顺序

```
v1（简单，先跑通）
  ├─ override 值直接存在 RenderObject 上（不用 SoA）
  ├─ 动画 tick 在 UI 线程
  ├─ 直写 API = NodeRef.write(closure)，一次 FFI，作用域 revoke
  ├─ 只支持 paint-only 属性
  └─ 不暴露裸指针

v2（游戏工具链需要吞吐时再上）
  ├─ AnimBuffer SoA + 双缓冲
  ├─ 渲染线程读 override，动画 tick 移到渲染线程
  ├─ 暴露裸指针给批量推送
  └─ API 不变 —— 因为宿主一直只用 NodeRef，从没直接碰过 struct
```

**关键：v1 和 v2 的 API 完全相同。** 这就是为什么 §14.5 要坚持「宿主永远不直接碰 struct」——它是让这条演进路径成立的唯一原因。

---

## 15. GacUI 的对照分析

> 来源见本文件末尾「参考」。以下基于 GacUI 官方仓库 README、gaclib.net 文档与 DeepWiki 的二手整理，**未逐行核对源码**，具体类名/接口请以仓库为准。

### 15.1 GacUI 的架构概览

GacUI 的核心是**四层概念 + 三个基础设施**：

```
概念层（自下而上）
  Element       基本图元：矩形 / 文本 / 图片 / 渐变 ...
  Composition   排版树。布局本身是一棵树
  Control       逻辑控件
  Template      皮肤。一个 Composition 子树

基础设施
  ① Vlpp 反射      DescriptableObject / TypeManager / IPropertyInfo / IMethodInfo
  ② Workflow 脚本  强类型、无 GC、可完整翻译成 C++
  ③ XML 资源       GacGen.exe 编译，最多 9 趟
```

关键：**「把控件放进布局时，实际是把该控件所控制的布局图元子树的根节点放进去」**——也就是说 GacUI 的 Control 和它的视觉子树是**分离的两棵树**。

### 15.2 ⭐ GacUI 独立收敛到了同一个设计（重要的验证）

vczh 明确的设计哲学是 **「decouple rendering from controls」**：早期尝试把控件、布局、绘图 API 揉在一起，结果复杂度爆炸；于是**把绘图从控件里彻底剥离**，抽象出窗口和皮肤接口，**每个元素有自己的 renderer 对象，系统资源缓存在 renderer 里**。

对照本文的建议：

| GacUI（约 2013） | 本文建议（§3 / §14） | 是否一致 |
|-----------------|---------------------|---------|
| Control 的**表示和逻辑分离** | Element / RenderObject 分离 | ✅ 同一洞见 |
| **每个 Element 有自己的 renderer**，系统资源缓存在 renderer | RenderObject 持 GPU 资源句柄 | ✅ 同一洞见 |
| `RenderTargetChangedInternal` 在 D3D device lost 时重建依赖资源 | GPU 资源生命周期 + device lost 恢复 | ✅ 必须做，他们做了 |
| Template / 皮肤接口与 Control 分离 | 主题/皮肤系统 | ✅ 同一洞见 |
| Composition 树（布局树）与 Control 树分离 | Element 树 / RenderObject 树 | ✅ 双树结构 |
| Core / Renderer 分离（**甚至跨进程**） | L2/L3 分层 | ✅ 他们走得更远 |
| Direct2D 1.1 + `IDXGISwapChain1` + `ID2D1DeviceContext` | D3D12 + 自研 RHI | ✅ 都是真 GPU retained |
| 每元素 renderer + `CachedSolidBrushAllocator` 等资源缓存分配器 | DrawBatch + 资源池 | ✅ 同一洞见 |

**这是很强的验证**：vczh 在 Flutter 出现之前就得出了「逻辑与表现分离 + 每元素持自己的 GPU 资源 + 保留资源跨帧复用」的结论。§3 和 §14 的方向是对的。

### 15.3 ⭐⭐ 决定性的分歧：反射

**GacUI 的整个上半身建筑在反射之上：**

```
XML 属性 → Value 装箱 → TypedValueSerializer → 反射 setter
Workflow 调用 → 反射方法查找 → 动态调用
数据绑定 → 反射属性访问
界面之外的语言 → FFI → Workflow 接口 → 反射
```

`DescriptableObject` / `TypeManager` / `Value` / `IPropertyInfo` / `IMethodInfo` / 各种 Serializer / 动态调用 —— 这是一个 GacUI 级别的独立子系统。

**而这正是 TacUI 决定不做 XAML 之后不需要的东西。**

### 15.4 ⭐⭐⭐ 最有价值的一条：vczh 自己放弃了反射

搜索结果里有一条极关键的记载：

> 早期单纯用反射加载 XML 导致 **exe 体积膨胀、启动速度慢**。解决方式是……更彻底的做法是**把 Workflow 脚本翻译成 C++，从而完全脱离反射**——既提升性能，崩溃也能用 VC++ 调试。

**一个花了十年造出最精密反射驱动的 C++ UI 框架的人，最终得出结论：反射是要被消除的成本。**

这条直接支撑 §13.3 的论证——**我们不是「因为做不到所以不做反射」，而是「因为它是负资产所以不做」**。而且 vczh 是付出代价之后才得到的结论。

注意他消除了**运行期**反射（改成 AOT 编译），但仍然保留了 XML + Workflow 这套**声明层**。这就是 TacUI 和 GacUI 的哲学分岔点。

### 15.5 三个基础设施：TacUI 该拿多少

| 基础设施 | GacUI | TacUI | 判断 |
|---------|-------|-------|------|
| **反射** | 核心承重墙 | 完全不要 | ✅ 该省 |
| **声明层** | XML + Workflow（AOT 编译成 C++） | 各宿主语言原生 DSL | ✅ 哲学分岔，TacUI 的选择降低了采纳门槛 |
| **多语言** | 在 Workflow 里声明接口 → 生成 → **在另一个进程用别的语言实现** → FFI/RPC | 原生 idiomatic 绑定 | ⚠️ 路线根本不同。GacUI 的「FFI 集成」至今标注「开发中」；本文 §5 的方案是主流路线 |

### 15.6 GacUI 值得抄的部分（高价值）

| # | 抄什么 | 为什么 | 对应本文 |
|---|-------|-------|---------|
| 1 | **`IGuiGraphicsElement` 图元设计** | 定义「Element 层暴露哪些原语」。GacUI 选了矩形/文本/图片/渐变等一小组 | §3.2 DrawBatch 的原语集 |
| 2 | **Control ↔ Template 的皮肤接口设计** | 控件声明「我的模板必须提供这些部件」，模板穿什么由使用者定。C++ 原生的换肤答案 | §13.4「该从 WPF 拿走的东西」的主题系统 |
| 3 | **Renderer 资源生命周期 + device lost 恢复** | `RenderTargetChangedInternal` 实测过的机制 | §3.1 GPU 资源常驻的落地细节 |
| 4 | **文本编辑器 / 文档模型** | 开源 C++ 里最完整的实现之一。桌面 App 和游戏工具都要 | M7 硬骨头 |
| 5 | **文本渲染管线**（字体回退链、Uniscribe/DirectWrite 切换） | 换行、bidi、fallback 的实战经验 | §3.4 |
| 6 | **虚拟列表 / MVC 容器控件** | 大列表性能的成熟解法 | M5 控件库 |
| 7 | **Composition 树的布局协议** | 他们也是「布局是一棵树」 | §4 布局 |
| 8 | **Core/Renderer 跨进程分离** | 极端的分层，即使不做也值得读懂边界划分 | §7 线程模型 |

### 15.7 GacUI 不要抄的部分

| # | 不抄什么 | 原因 |
|---|---------|------|
| 1 | **Vlpp 反射** | 见 §15.4。连作者都把它从运行期移除了 |
| 2 | **XML + Workflow + GacGen（9 趟编译）** | 你明确不要 XML；且这是复杂度的主要来源 |
| 3 | **可变对象模型（无 diff）** | 封死热重载；跨语言只能靠反射/RPC |
| 4 | **多语言走「另一进程 + RPC」** | 与「各语言地道 DSL」的目标路线冲突 |
| 5 | **继承 + XML 模板重写的扩展模型** | 即 §11.3 的路线 A，对非 core 语言不友好 |

### 15.8 GacUI 提供的最重要东西：工程量现实检查

搜索结果里几条时间线值得记住：

- XML Resource 约 **2013 年**成形，**2018 年**功能固定，共经历 **5 个版本**
- `GacGen.exe` 有 **最多 9 趟预编译**
- 作者是 **MSVC 编译器团队的工程师**，项目持续 **10+ 年**
- 结果：技术极其出色，**基本没有商业采用**

**这是 §10 风险 #1（范围）的实证。** 一个人能写出最精密的 C++ UI 框架之一，但写不出生态。这不是能力问题，是**范围问题**——UI 库的工作量根本不在渲染内核。

对 TacUI 的启示：
1. **必须有明确的场景收窄**（§12 定的「桌面 App + 游戏工具链」还不够窄）
2. **控件库、文本、平台集成才是主战场**，渲染内核只是入场券
3. **反射 / XML / 脚本语言这三个子系统省下来，是 TacUI 相对 GacUI 最大的结构性优势**——省下的不是 10%，是可能一半的工程量

### 15.9 TacUI 相对 GacUI 的赌注差异（一句话）

| | GacUI | TacUI |
|---|-------|-------|
| **赌注** | 一个富运行期 + 反射驱动的绑定 / 脚本 / XML 资源 | 一个薄核心 + 宿主语言原生的人机工效 |
| **哲学归属** | Java / C# / WPF 那一系（「运行时提供一切」） | Flutter / SwiftUI 那一系（「核心尽量薄，工效交给宿主语言」） |
| **代价** | 用户要学 Workflow + XML；跨语言靠 RPC | 核心要写 diff 引擎；扩展模型受限于 C ABI |

**GacUI 的赌注在 2013 年是合理的，在 2026 年不合理**——因为「让宿主语言做它擅长的事」现在有 Flutter/SwiftUI/Compose 三条成功先例，而且宿主语言生态的引力比任何自造脚本语言都强。

### 15.10 结论

> **GacUI 有极高的参考价值，但主要不在整体架构，而在两个地方：**
> **(a) 特定子系统的成熟解法**——图元设计、皮肤接口、renderer 资源生命周期、文本编辑器、虚拟化容器（§15.6）
> **(b) 「不该做什么」的反面教材**——反射、XML+脚本、可变对象模型（§15.7），以及最重要的：**范围失控的实证**（§15.8）
>
> **而在「逻辑与表现分离 + 每元素持 GPU 资源」这个核心洞见上，GacUI 独立收敛到了与 §3/§14 相同的答案——这是对本文方案最强的外部验证。**

---

## 参考

- [vczh-libraries/GacUI — README](https://github.com/vczh-libraries/GacUI)
- [GacUI README (raw)](https://raw.githubusercontent.com/vczh-libraries/GacUI/refs/heads/master/README.md)
- [Windows Native Implementation — DeepWiki](https://deepwiki.com/vczh-libraries/GacUI/4.1-windows-native-implementation)
- [Vlpp Reflection and Type System — DeepWiki](https://deepwiki.com/vczh-libraries/GacUI/2.4-vlpp-reflection-and-type-system)
- [Serialization and Dynamic Invocation — DeepWiki](https://deepwiki.com/vczh-libraries/GacUI/9.2-serialization-and-dynamic-invocation)
- [Text Rendering Pipeline — DeepWiki](https://deepwiki.com/vczh-libraries/GacUI/7.4-text-rendering-pipeline)
- [OS Provider — gaclib.net](http://gaclib.net/doc/current/gacui/kb/osprovider.html)
- [GacUI: XML Resource（含「脱离反射」的历史记载）](https://www.e-com-net.com/article/1404100603868663808.htm)
- [GacUI: 跨平台和渲染器](https://www.e-com-net.com/article/1398669229842026496.htm)

---

## 16. WinUI 分析：它是第三种模型吗？

> 来源见本文件末尾「参考」。DP 性能数字来自厂商博客，**可能随版本变化**，请自行复核。

### 16.1 直接结论

> **WinUI 不是第三种模型。它是 WPF 的编程模型 + 一个现代的合成引擎。**
> **而它的编程模型部分比 WPF 更差，不是更好**——因为每次 DP 访问都要付 WinRT interop 税。

但它有一个**极其优秀**的部分（Visual Layer），以及两个对 TacUI 有直接价值的发现。

### 16.2 WinUI 的实际架构：引擎 ≠ 编程模型

必须把两者拆开看：

| | 编程模型 | 引擎 |
|---|---------|------|
| **内容** | DependencyObject / DependencyProperty / XAML / Style / Binding / Measure-Arrange | XAML Core Engine（`dxaml/xcp`）+ Composition Visual Layer |
| **评价** | **与 WPF 同源，且更慢** | **现代、优秀，值得深入学习** |

**引擎侧的实际情况：**
- XAML Core Engine 是**原生 C++**（Windows XAML 从 Windows 8 起就是 C++）
- **DXaml**（Direct XAML）提供 WinRT 投影，把公开对象映射到内部 core 对象
- **XCP（Core）**负责布局循环、焦点管理、输入路由
- **`UIElement` 映射到 Composition Visual**，引擎做一次 **"render walk"** 把 XAML 视觉树同步到 `Microsoft.UI.Composition`
- 底层是 **DirectX 11/12**，**retained-mode，60fps 目标，增量布局**（属性变化只重算受影响部分）

### 16.3 ⭐ 发现一：DP 在 WinUI 里比 WPF 慢 30–150 倍

因为 `DependencyObject` 映射到 WinRT 对象（`IDependencyObject`），**每次 getter/setter 都要付 interop 成本**：

| 操作 | WPF | WinUI | 倍数 |
|-----|-----|-------|-----|
| `GetValue` | ~19 ns | ~2023 ns | **~106x** |
| `SetValue` | ~135 ns | ~4272 ns | **~32x** |
| `SetValue` + 变更回调 | ~139 ns | ~20431 ns | **~147x** |

⚠️ 数字来自 DevExpress 厂商博客，可能版本相关，但方向明确，且是已知的 GitHub issue。

**为什么这条对 TacUI 极其重要：**

DP 系统的**全部卖点**是「细粒度失效省下工作量」。但如果每次属性访问要 2–4 微秒，**你辛苦省下的重算时间被访问成本吃回去 100 倍**。

**更精确的教训：** 这个税来自**动态/interop 边界**——而这正是反射驱动的 DP 所必需的。如果我们做 DP，为了服务跨语言场景，同样需要那层动态边界，就会掉进同一个坑。

**这是对我们 C ABI 的直接警告：** 我们的 `capi` 层和 WinUI 的 WinRT 层是**同构的边界**。任何「每次属性变更一次跨界调用」的设计都会重演这个灾难。§14.8 的 v2 SoA buffer、以及 §5.2 的批量提交，不是可选优化，是**必须做对的地方**。

### 16.4 ⭐⭐ 发现二：`Translation` vs `Offset` —— base/override 的现实验证

WinUI 的官方文档里有一条 gotcha：

> **优先用 `Translation` 而不是 `Offset`**：XAML 会用自己算出的值覆盖 Composition 的 `Offset`，但**它不知道 `Translation`**，所以 `Translation` 是**叠加的、不会被覆盖**。`Translation` 只对由 `FrameworkElement` 创建的 Visual 有效。

**翻译成我们的语言：** 微软撞上了**和 §14.3 一模一样的问题**——当声明层（XAML）和直写层（Composition 动画）都要写「位置」这个属性时，谁赢？

**他们的答案是：给 override 单独一个属性名（`Translation`），让它叠加、永不被覆盖。**

这**正是 base / override 分离**，微软是被迫独立发明出来的。§14.3 的设计获得了最强的现实验证。

### 16.5 ⭐ 发现三：Visual Layer 的动画不依赖 UI 线程

> Visual Layer 的动画系统 **"framework-agnostic and designed from the ground up for performance"，独立于 UI 线程运行**。

- 效果（模糊/阴影/透明度）同样脱离 UI 线程渲染
- `CompositionPropertySet`（`GetScrollViewerManipulationPropertySet`、`GetPointerPositionPropertySet`）+ `ExpressionAnimation` → **60fps 动画不受 UI 线程阻塞影响**
- **`ElementCompositionPreview.GetElementVisual(element)`** → 拿到任意 XAML 元素的 Visual 直接驱动
- WinUI 3 更进一步：`UIElement.StartAnimation(...)` 直接调

**这验证了两件事：**
1. §4 说的「官方暴露直写通道」是对的——WinUI 把这个做成了 public API
2. §14.7 里我列为 **v2** 的「动画 tick 移到渲染线程」，WinUI 在 OS 合成器层面已经做到了。**这是我们 v2 该瞄准的目标，不是可选项**

### 16.6 顺带发现：微软自己也在逃离 DP 模型

`microsoft-ui-reactor` 是一个官方实验项目，明确地**打破 XAML DP 模型**：

- binding → **闭包 over state**
- style → **modifier 组合**
- VisualStateManager → **interaction-state modifier**

**这就是 Flutter / SwiftUI / Compose 的模型，由微软自己做出来的。** 这比任何第三方论证都有分量——**微软内部也认为 DP 模型是可以被替换的。**

### 16.7 所以 WinUI 作为编程模型，够优秀吗？

**不够。** 理由按重要性：

1. **它是 WPF 的模型**，§13 的全部分析原样适用（需要反射、需 XML 才有意义、可变对象、无热重载）
2. **它的 DP 比 WPF 慢 30–150 倍**——DP 模型的核心卖点在自己的实现里被抵消了
3. **生态碎片化地狱**：UWP XAML / WinUI 2 / WinUI 3 / WPF / Windows App SDK / `Windows.UI.Xaml` vs `Microsoft.UI.Xaml`，命名空间十年churn，MSIX 打包要求，`WindowsAppSDK` 运行时依赖
4. **部署故事痛苦**：WinUI 3 应用要带 Windows App SDK runtime
5. **微软自己用 Reactor 在逃离它**
6. **它不是一个可以搬走的模型**——与 XAML / .NET / COM / WinRT 深度绑定，无法移植到跨语言库

**但 WinUI 的引擎侧值得学，且已经验证了我们的方向**（§16.3–16.5）。

### 16.8 如果你真的不想用 Flutter 风格：第三条路存在

在「Flutter 式（不可变描述 + diff）」和「WPF 式（DP + 反射）」之外，确实有第三条路——**接近 WinUI 实际架构的那条**：

```
① 声明式 DSL 只求值一次 → 构建一棵可变的 Element 树
   （不是每帧重建，是模板式的一次性构建）

② 状态变化不重建树，而是通过显式绑定传播
   text.bind(count, |c| format!("count = {c}"))     ← 订阅 + 重算单个属性
   （无反射：绑定是类型化的闭包）

③ 直写通道驱动 GPU 合成节点（即 §14 的 override）
```

**这条路的真实取舍：**

| | Flutter 式 | 第三条路（WinUI 架构 - DP + 反射） |
|---|---|---|
| **diff 引擎** | 必须（2–3 人月） | **不需要**，省掉 |
| **arena 分配压力** | 有（每次重建分配） | **无**，树构建一次 |
| **条件结构** `if c {A} else {B}` | diff 自动处理 | ⚠️ **弱点**。需要显式重建边界，最终会重新发明**局部 diff** |
| **动态列表** | diff 自动处理 | ⚠️ 需要可观察集合（`ObservableCollection` 那套） |
| **热重载** | ✅ 几乎免费 | ❌ 失去 |
| **「UI = f(state)」纯度** | ✅ | ❌ 变成双向，可预测性下降 |
| **可测试性** | ✅ 树可独立构造断言 | ⚠️ 需要真实运行时 |
| **新增的规则** | 3 个概念 | +订阅 +显式重建边界 +可观察集合（**概念更多**） |

**这条路的陷阱：** 你为「省掉 diff」付出的代价，是**在每个用到条件结构和动态列表的地方手动做局部 diff**。而条件结构和动态列表在真实 UI 里到处都是。最后你很可能发现：**局部 diff 拼起来就是一个更差、更不统一的全局 diff。**

**第三条路在什么情况下是对的：**
- 你的 UI 结构**基本静态**，变化集中在属性值上（比如仪表盘、监控面板、图表）
- 你**不需要热重载**（C++ core + 多语言宿主下热重载本来就极难）
- 你愿意接受「条件结构用显式重建边界」这个语法负担

**对「桌面 App + 游戏工具链」来说：不适用。** 这两类应用的界面结构恰恰是高度动态的——dock 面板可增删、属性面板随选中对象变化、列表虚拟化、节点图增删节点。

### 16.9 一个可能更重要的判断

**你可能不喜欢 Flutter 风格的地方，§14 的设计已经解决了。**

Flutter 风格最让人不适的是「每帧重建整棵树」的心智模型。但 §14 的 base/override 设计**本来就把这个问题解决了**：

```
路径 A · 声明式   改 state → 重建 → diff      ← 只对「结构变化」付代价
路径 B · 直写     写 override → 零重建         ← 高频变化走这里
```

**「直写通道」就是你想从 WinUI 那里要的东西。** WinUI 给它起名叫 `Translation` / Visual Layer；我们给它起名叫 override / `NodeRef.write()`。**机制是同一个。**

**所以你不需要在「Flutter 风格」和「WinUI 风格」之间选——§14 已经两个都要了。** 决策已经下移到**单个属性**的层面：高频属性走直写，结构变化走重建。

**如果你不喜欢的是别的东西**（比如不可变对象的分配成本、泛型 DSL 的写法、组合导致的深树），那是另一个问题，需要具体说，因为解法完全不同。

### 16.10 结论

| 问题 | 答案 |
|-----|------|
| WinUI 是第三种模型吗？ | ❌ 不是。是 WPF 模型 + 现代引擎 |
| WinUI 作为编程模型够优秀吗？ | ❌ 不够。是 WPF 模型，且 DP 比 WPF 慢 30–150x |
| WinUI 有值得学的吗？ | ✅ **有，而且很关键**（§16.3–16.5） |
| 它的引擎验证我们的方案吗？ | ✅ **验证了 §3 / §14，且提供了两条硬证据** |
| 该改用 WinUI 风格吗？ | ❌ 不该。但如果对 Flutter 风格有具体顾虑，见 §16.8–16.9 |

---

## 参考（WinUI）

- [WinUI Architecture — DeepWiki](https://deepwiki.com/microsoft/microsoft-ui-xaml/2-winui-architecture)
- [WinUI 3 Performance Boost — DevExpress（DP 性能数字）](https://community.devexpress.com/blogs/wpf/archive/2022/01/24/winui-3-performance-boost.aspx)
- [Reactor vs XAML — microsoft-ui-reactor](https://microsoft.github.io/microsoft-ui-reactor/0.1.0-preview.12/reactor-vs-xaml/)
- [Visual Layer — windows-dev-docs](https://github.com/MicrosoftDocs/windows-dev-docs/blob/docs/hub/apps/develop/composition/visual-layer.md)
- [XAML and Composition Interoperability — Microsoft Learn](https://learn.microsoft.com/ka-ge/windows/apps/develop/composition/xaml-comp-interop)
- [New Lights and PropertySet Interop — Windows Developer Blog](https://blogs.windows.com/windowsdeveloper/2017/07/19/new-lights-propertyset-interop-xaml-visual-layer-interop-part-two/)
