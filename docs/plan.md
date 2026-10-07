# TacUI 方案

> 本文件是**决策与执行方案**。推理论证过程见 [architecture.md](architecture.md)，本文件只给结论。
> 状态：v1 草案，待拍板项见 §8

---

## 0. 结论（TL;DR）

### 0.1 架构已定，且被四个独立来源交叉验证

```
Flutter 的三棵树（不可变描述 + diff + 稳定身份）
        +
WPF 的直写快路径（base / override 双层属性 + 显式 token）
```

这**不是一个折中方案，而是所有验证过的做法的交集**：

| # | 来源 | 验证了什么 |
|---|------|-----------|
| 1 | **GacUI**（~2013，早于 Flutter） | 逻辑与表现分离 + 每元素持自己的 GPU 资源 |
| 2 | **WinUI 的 `Translation` vs `Offset`** | base / override 必须分离，微软被迫独立发明 |
| 3 | **WinUI 的 Visual Layer** | 官方暴露直写通道；动画脱离 UI 线程 |
| 4 | **Flutter / SwiftUI / Compose** | 不可变描述 + diff + 稳定身份树 |

四个独立来源指向同一个设计。**架构这块没有悬念了。**

### 0.2 各决策的信心等级（诚实标注）

| 决策 | 信心 | 依据 |
|-----|------|------|
| 三棵树 + diff | 🟢 高 | 四个独立来源 |
| base / override 直写通道 | 🟢 高 | WinUI 被迫独立发明（`Translation`） |
| 不做反射 | 🟢 高 | vczh 用十年换来的教训 |
| C ABI 作为唯一边界 | 🟢 高 | 行业标准 + C header 本身就是半个代码生成器 |
| 不做 DependencyProperty | 🟢 高 | 无 XML 时性价比极低；WinUI 的 DP 慢 30–150x |
| 解析 AA 进 M1 | 🟢 高 | 节点图是游戏工具链核心场景 |
| **C++ 作为 core 语言** | 🟡 **中** | 场景权衡，非技术定论。可用 M0 双原型终结 |
| **扩展模型「组合优先」** | 🟡 **中** | 依赖 Q7，尚未拍板 |
| **场景边界** | 🔴 **低** | ⚠️ **尚未解决，且是最大风险** |

**注意力应该放在信心低的三行上，而不是继续打磨信心高的六行。**

### 0.3 但真正的结论是：架构从来不是这个项目的风险

我们对**每一个架构问题都找到了可辩护的答案**——这恰恰说明架构部分已经被行业趟明白了。

而 GacUI 的实证是：

> **顶尖工程师（MSVC 编译器团队）· 10 年 · 架构完全合理 · 零商业采用。**

架构正确，不保证项目成功。因为 **80% 的工作量不在架构里**：

```
渲染内核         ← 我们这几轮讨论的全部内容    ≈ 20%
控件库 + 文本 + IME + 无障碍 + 平台集成        ≈ 80%
```

**决定成败的不是「三棵树还是两棵树」，是「第一个真实目标是什么」和「砍掉什么」。**

### 0.4 所以下一步只做一件事：收窄场景

不要再继续做架构分析了。**现在应该做的是：**

1. **选定一个具体的、你要真正替换掉的第一个工具**（不是「游戏工具链」这个集合，是其中一个具体的窗口）
2. **列出它需要的全部控件，数出来** —— 这个数字就是真实的预算表
3. **如果 v1 需要的控件 > 20 个 → 场景还要再窄**

做完这三步，M3–M6 的控件清单就能排出来，然后才开始写 M0。

**M0 的六条验收标准见 §6.1；架构赌注的证伪门 G0 见 §9.2。这两个是开工前必须过的东西。**

---

## 1. 一句话定位

**一个 GPU 加速、retained mode、C++ 核心、可在任意宿主语言里用原生声明式语法编写 UI 的界面库。**

结构上参考 GacUI 的「逻辑与表现分离 + 每元素持 GPU 资源」，但**砍掉 GacUI 的反射系统、XML 资源和 Workflow 脚本**，改由宿主语言自身承担声明式表达。

---

## 2. 锁定的技术决策

| # | 决策 | 理由 | 详见 |
|---|------|------|------|
| D1 | **Core 用 C++** | 游戏工具链需要自定义绘制控件 + 引擎交换链互操作；受众是 C++ 团队 | §11.4 |
| D2 | **三棵树：VNode / Element / RenderObject** | 无反射、跨语言一致、可热重载、可测试 | §2 |
| D3 | **不可变描述 + diff**（Flutter 式），而非可变对象原地改（WPF 式） | WPF 式需要反射；且其优势依赖 XML，我们不做 XML | §13.9 |
| D4 | **base / override 双层属性 + 显式 token**（WPF 的直写快路径） | 动画和外部数据推送零重建；只有一层优先级规则，不需要 DP 系统 | §14.2–14.3 |
| D5 | **明确不做 DependencyProperty 系统、不做反射** | 无 XML 的前提下性价比极低；vczh 自己也把反射从运行期移除了 | §13.3 / §15.4 |
| D6 | **布局用 `BoxConstraints` 值类型**（Flutter 式），非自由 `MeasureOverride` | 更严格，且纯值类型对 C ABI 友好 | §13.2 |
| D7 | **C ABI 是唯一跨语言边界**，代际句柄，IDL 驱动代码生成 | 稳定、可批量、各语言可自动化 | §5 |
| D8 | **宿主永远不直接碰内部 struct**，只拿 `NodeRef` | 让 v1→v2 演进（SoA buffer、渲染线程）成为 drop-in | §14.5 / §14.8 |
| D9 | **扩展模型：组合优先 + 保留 C++ 子类化** | 组合让所有语言体验一致；C++ 子类化保住性能上限 | §11.3 |
| D10 | **DSL 路线 C：core 提供 builder 基座，各语言写薄的原生 DSL** | 平衡体验与维护成本，用 parity test 保证语义一致 | §6 |
| D11 | **第一版只做 Windows + D3D12** | 收敛平台层工作量（多平台约 1.5x） | §3.5 |
| D12 | **解析 AA 必须进 M1**（不能推迟） | 节点图连线是贝塞尔曲线，是游戏工具链的核心场景 | §12 |

---

## 3. 架构总览

```
┌─ 宿主语言 ── Rust / C# / Python / Swift / TS ─────────────────┐
│  原生 DSL（macro / source-gen / context manager）             │
│    ↓ 构造 VNode（arena 分配，一次 FFI 提交整棵子树）            │
├─ C ABI ── 唯一的稳定边界（句柄代际索引 / 回调表 / 版本协商）────┤
├─ Framework (C++) ────────────────────────────────────────────┤
│  reconcile → Element 树（稳定身份 + state 槽 + token）         │
│    ↓ create / update / destroy                                │
│  RenderObject 树                                              │
│    ├── base      ← reconcile 写（低频，事件驱动）              │
│    ├── override  ← 直写通道写（高频，每帧）                     │
│    └── Measure / Arrange（只被 base 变化触发）                  │
├─ Scene (C++) ── DrawBatch / GPU 资源句柄 / 脏标记 ────────────┤
├─ Renderer (C++) ── 批处理 / Analytic AA / Glyph Atlas ────────┤
└─ RHI + Platform (C++) ── D3D12 / 窗口 / 输入 / IME / DPI ─────┘
```

**两条变更路径（本方案的核心）：**

```
路径 A · 声明式      改 state → 重建 VNode → diff → 更新 base → 必要时重排/重绘
路径 B · 直写        写 override（animation / 命令式）→ 只重绘，零重建
生效值 = override(若 token 活跃) 否则 base
```

---

## 4. 对外 API 长什么样

### 4.1 路径 A：声明式

```rust
// Rust —— proc-macro，体验对标 SwiftUI
view! {
    VStack(spacing: 8) {
        Text(format!("count = {}", count.get())).font_size(24)
        Button("+1").on_click(move |_| count.set(count.get() + 1))
    }
    .padding(16)
}
```

```python
# Python —— context manager + builder
with vstack(spacing=8).padding(16):
    text(f"count = {count.get()}").font_size(24)
    button("+1").on_click(lambda: count.set(count.get() + 1))
```

```csharp
// C# —— collection initializer + fluent
VStack(spacing: 8, padding: 16) {
    Text($"count = {Count}").FontSize(24),
    Button("+1").OnClick(() => Count++),
}
```

**三者必须产出完全相同的 VNode 树** —— 用 `tests/dsl_parity/` 的 golden test 验证。

### 4.2 路径 A′：声明式动画（语法糖 → 编译成路径 B + token）

```rust
Rect()
    .size(100, 100)
    .translate(tx.animate(spring(0.0, 1.0)))   // 走直写，不重建
```

### 4.3 路径 B：命令式直写

```rust
// 游戏编辑器的 C++ 模型层推数据进来
let node: NodeRef = scene.get_ref("marker_42");
node.write(|w| {              // 一次 FFI，作用域结束自动 revoke
    w.translate(120.0, 88.0);
    w.opacity(0.6);
});
```

```c
// C ABI 层（IDL 生成的形状）
tui_node_ref_t node = tui_scene_get_ref(scene, "marker_42");
tui_write_begin(node);
tui_write_f32(node, TUI_PROP_TRANSLATE_X, 120.0f);
tui_write_f32(node, TUI_PROP_TRANSLATE_Y,  88.0f);
tui_write_f32(node, TUI_PROP_OPACITY,       0.6f);
tui_write_commit(node);       // 离开作用域 → revoke
```

### 4.4 v1 直写只允许 paint-only 属性

| 可直写 | 必须走重建 |
|-------|-----------|
| translate / scale / rotate / opacity / color / paint 层 offset | 所有影响布局的属性（size / padding / alignment / 可见性） |

**这条守不住，整个快路径要返工。** 布局缓存会被打乱。

---

## 5. 仓库结构

```
tacui/
├─ docs/            architecture.md（分析）· plan.md（本文件）
├─ idl/             接口定义 —— 所有绑定的单一真源
├─ core/
│  ├─ base/         句柄 / 代际索引 / arena / 几何 / SmallVector
│  ├─ framework/    Element 树 / diff / state 槽 / 控件
│  ├─ scene/        Layer 树 / DrawBatch / 脏标记
│  ├─ renderer/     批处理 / Analytic AA / Glyph Atlas / 路径
│  └─ text/         shaping / 布局 / 缓存
├─ rhi/
│  ├─ include/      后端无关接口
│  └─ d3d12/        第一版唯一后端
├─ platform/windows/ 窗口 / 输入 / IME / DPI / UIA
├─ capi/            C ABI 边界层（唯一导出符号）
├─ bindings/        rust/ csharp/ python/ swift/ kotlin/ ts/
├─ tools/
│  ├─ idlgen/       IDL → 各语言代码生成
│  └─ inspector/    ⭐ 视觉树 / 帧分析 / overdraw / override 可视化
├─ tests/
│  ├─ golden/       截图回归（含 GPU 差异容差）
│  └─ dsl_parity/   跨语言 DSL 语义一致性
└─ examples/
```

**`tools/inspector` 从 M0 就要开始做。** 没有可视化调试器的 UI 库，开发效率低一个数量级。

---

## 6. 里程碑

| 阶段 | 内容 | 验收标准 |
|-----|------|---------|
| **M0 架构验证** | Win32 窗口 + D3D12；圆角矩形 + 文本；Element 树 + 最小 diff；直写通道；**最小 C ABI + 一个非 C++ 语言的绑定** | 见 §6.1 |
| **M1 渲染内核** | RHI 抽象、batcher、**解析 AA（含贝塞尔）**、glyph atlas、帧快照、device lost | 复杂界面稳定 60fps；节点图连线不糊 |
| **M2 Framework** | 完整 reconcile（key/列表重排）、state 槽、BoxConstraints 布局、10 个基础控件、动画系统、主题 | 能写可交互 Demo |
| **M3 控件库 I** | 30+ 控件、虚拟化列表/树/表格、dock 布局 | 能搭出真实工具界面 |
| **M4 绑定 + DSL** | Rust 绑定 + `view!` 宏；Python；dsl_parity 基建；Inspector 可视化 | 两语言语义一致 |
| **M5 硬骨头** | 文本编辑器、IME、多窗口/弹窗、无障碍 | —— |
| **M6 多语言铺开** | C# / TS / Swift / Kotlin | 全部通过 parity test |

### 6.1 M0 的精确验收标准

M0 **不是** "Hello World 按钮"。M0 的任务是**验证架构里最冒险的假设**。必须同时满足：

1. ✅ D3D12 画出圆角矩形 + 一行文本，**顶点缓冲和 atlas 跨帧复用**（证明 retained 成立）
2. ✅ 一个 counter：点按钮 → `setState` → 重建 VNode → diff → 文本正确更新（证明路径 A 成立）
3. ✅ 一个方块：动画直接改 translate，**零重建**（证明路径 B 成立，用重建计数断言）
4. ✅ **两条路径作用在同一节点上互不干扰**：DSL 写 `.opacity(1.0)` + 动画覆盖 `opacity` → 动画生效；动画结束 → 回落。reconcile 不打断动画（证明 D4 成立）
5. ✅ **一个非 C++ 语言能驱动它**（哪怕只是 Python 的最小 ctypes 绑定画出同一个界面）—— 证明 C ABI 形状可用
6. ✅ Inspector 能打印 Element 树 + 每节点的 base/override 值

**第 4 条和第 5 条是 M0 的真正价值。** 前三条任何人都会做对；4 和 5 是本方案特有的架构赌注，越早证伪越好。

预估：**1–2 个月**（1 人）。

#### 实施进度

| # | 验收标准 | 状态 | 证据 |
|---|---------|------|------|
| 1a | D3D12 圆角矩形 + 解析 AA | ✅ | `m0 --capture` → PNG |
| 1b | 顶点/实例资源跨帧复用 | ✅ | 常驻环形缓冲，descriptor 只建一次 |
| 1c | 文本渲染（DirectWrite + glyph atlas） | ✅ | 91 次光栅化支撑 959 帧；`textprobe` ASCII 验证 |
| 2 | setState → 重建 → diff → 显示更新 | ✅ | `--selftest`：1920 帧 / 64 次重建 |
| 3 | 动画直写，零重建 | ✅ | `rebuilds<<paints`，动画帧零重建 |
| 4 | 两路径同节点共存，重建不打断 override | ✅ | `survivedRebuild=63`，值从未被覆盖 |
| 5 | 非 C++ 语言驱动 | ✅ | Python/ctypes 宿主，含文本，`demo.py --selftest` 全 PASS |
| 6 | Inspector 打印树 + base/override | ✅ | `tui_dump_tree` → 每节点 base/override/文本内容可见 |

**M0 全部完成。** 文本层 M0 范围：单一字体、码点→字形 1:1（无 ligature / 复杂文种）、单行不换行。HarfBuzz + ICU 在 M1 替换（architecture.md §3.4）。

**已建成的基建：** D3D12 校验层诊断通道（stderr）、帧回读 → PNG（golden-image 测试的种子）、`--selftest` 自动断言（C++ 与 Python 各一份）、`textprobe`（无需 GPU 的文本栈验证）。

**已抓到的真实缺陷（全部由基建而非肉眼发现）：**

1. instance base 以 `float` 传入声明为 `uint` 的 root constant（位模式被重解释 → 越界读 0 → 几何退化）。**校验层完全静默**，只有帧回读能发现。
2. 节点被替换类型时，旧 `OverrideToken` 仍会写新节点的 RenderObject。加了 `Element::generation` 做代际校验。
3. **`tui_run` 的首帧回调跑在树建立之前** —— 宿主在 frame 回调里解析句柄必然失败。**只有写第二个宿主语言才会暴露。**
4. `GetGlyphIndices` 要 UTF-32 而非 UTF-16；把 `wchar_t*` 直接 reinterpret 成 `UINT32*` 会静默返回 .notdef。
5. `GetAlphaTextureBounds` 在纹理类型与 rendering mode 不匹配时**返回空矩形而非报错**；实测只有 `CLEARTYPE_3x1` 能出 ink（与文档直觉相反）。
6. **atlas 每次重建都被完整重传**（1 MiB memcpy + 全纹理拷贝，32 次）。原因是标脏无条件。由 stats 发现，修后降到 10 次且不再随重建增长。
7. **`ui::Stats` 加了字段但 C ABI 的 `tui_stats` 没同步** —— 而 **ctypes 不校验结构体大小**，`tui_get_stats` 会径直写穿宿主侧的缓冲区。这是静默内存破坏。加了 `tui_stats_size()`，绑定在 import 时比对 sizeof，把静默破坏变成加载期报错。
8. **`liveOverrides` 只在 `update()` 里赋值**，而树不脏时 `update()` 直接 early-return，所以这个值会停在第一次构建时的状态。改为在 `paint()` 里统计（它本来就在遍历树）。
9. **文字发糊**：字形的 pen advance 是小数（排印正确），但按小数位置画位图会被线性采样器重采样。修法是**吸附字形原点到整像素、保留小数 advance**。间距不受影响（同一基线上的字形相对偏移恒为整数），字形变清晰。
10. **文字仍然发糊（#9 的续集，另一个原因）**：atlas 的 UV 用了"纹素中心内缩"（`(x+0.5)/size … (x+w-0.5)/size`），而 quad 横跨的是**像素边缘到边缘**（`center ± halfSize`）。两者不匹配 → 把 `w` 个纹素塞进 `w` 个像素，**每个像素都在双线性混合相邻纹素**，字形被系统性重采样、发软。改成边界到边界（`x/size … (x+w)/size`）后是严格 1:1 贴图。实测文字密集区域边沿能量提升约 **1.65x**，纯背景区域 0 像素变化；`textprobe` 现在断言 UV 跨度 == 位图尺寸。
11. **文字还是糊（#9/#10 之后的主因）**：进程**没有声明 DPI 感知**，在 150% 缩放的显示器上 Windows 把整个窗口位图拉伸 1.5 倍——一切都被重采样，文字最明显。注意陷阱：`GetDpiForSystem()` 从**非 DPI 感知**的进程里问会返回 96，所以一开始误判成 100%（`HKCU\Control Panel\Desktop\WindowMetrics\AppliedDPI` 才是真值 144）。修法：进程声明 Per-Monitor V2，窗口/后缓冲走物理分辨率，UI 保持逻辑单位，paint 时 ×scale，**字形按 `fontSize×scale` 在物理像素上光栅化**（而不是放大位图）。1x 截图哈希不变，证明对未缩放路径零影响。

**M0 之后的工作**见 [components.md](components.md)：输入系统（命中测试 / hover / press / 焦点 / Tab）、主题令牌、12 个 L2 控件、`ScrollView` / `ListView` 与相对布局（row / column）已完成（见 [布局](layout.md)）；其余 L2、通用约束传播、`Popup`、IME 待做。

**从 Python 宿主得到的 ABI 反馈：**
- 代际句柄（`{index, generation}`，8 字节）在 ctypes 下工作良好，`__bool__` 语义自然
- `user` 指针 + 宿主侧注册表是可行模式（比闭包更贴合 ABI 意图）
- **控件层与输入注入已导出**：`tui_label` / `tui_button` / … / `tui_panel` 让宿主语言用**同一套**控件；`textInput` / `scrollView` / `listView` 的跨帧状态由库按 key 托管（访问器读写）；`tui_dispatch_event` + `tui_update` 让宿主自己驱动事件与重建，从而能无窗口验证（`bindings/python/controls_test.py`）
- **构建面确实 chatty**：每节点 3~4 次 FFI。可接受，但大树的批量提交（§5.2）迟早要做
- **ctypes 回调里抛异常不会传播进 C 循环** —— 宿主的错误会静默丢失。ABI 需要一条回传错误的路径（M1）

#### 示例与宿主基建

M0 之后补了三样面向使用者的东西：

| 文件 | 作用 |
|-----|------|
| `include/tacui/` | **公开 API 的唯一边界**。`#include <tacui/tacui.hpp>` 就是全部 |
| `core/framework/builder.{h,cpp}` | C++ 的 fluent builder —— D10「core 提供 builder 基座」的落地。C++ 无可用宏，这是代价（§11.4） |
| `host/host.{h,cpp}` | 现成的 Win32 + D3D12 宿主外壳。core 保持可嵌入；`ui.update()` / `ui.paint()` 仍可手动驱动 |
| `examples/asset_browser/` | **同一界面的 C++ 与 Python 两版**，`main.cpp` 与 `browser.py` 并排可读 |

C++ 版实测：`rebuilds=1  paints=323`（323 帧、1 次重建），Python 版同构。

#### 公开 API 边界（新增约定）

```
include/tacui/     ← 公开。应用代码只能 include 这里
  tacui.hpp           C++ 入口（umbrella）
  tacui.h             C ABI 入口
  geometry/vnode/element/ui/builder/text_system/glyph_atlas/rhi/rhi_factory/host .hpp

core/ rhi/d3d12/ platform/ tools/ capi/*.cpp   ← 实现，不属于 API
```

**纪律：** `examples/` 与 `apps/` 只 include `<tacui/tacui.hpp>`。这条约定由 CI 检查（grep 非公开 include 即失败），因为一旦泄漏，重构内部结构就会破坏使用者。

`m0` 本身现在也只 include 公开入口 —— **harness 同时是「公开 API 是否够用」的测试**。

**帧循环收敛为一份：** 原本 `m0`、`capi`、`host` 各有一份几乎相同的 window+device+loop。现在三者都走 `host::run`，`host::run` 不做 `ui.shutdown()`（否则调用方无法在 run 之后检查树）。

**顺带修的 ABI 缺陷：** `tui_run` / `tui_capture_next_frame` 原本收 `const wchar_t*`，而 `tacui.h` 自己写着「字符串一律 UTF-8」。且 `wchar_t` 在 Windows 是 2 字节、Linux 是 4 字节 —— **ABI 根本不跨平台**。已统一为 UTF-8，与 header 声明的规则一致。

**M0 遗留的已知限制（进 M1 再解）：**
- ~~矩形与字形是两个独立的 draw，跨类型的绘制顺序不成立~~ —— ✅ **已解**：paint 现在按树序生成**有序 draw list**（同类型合并成 run，按提交顺序交错），`controls_gallery --paint-test` 直接断言交错顺序
- atlas 无淘汰；满了就丢字形并计数
- 灰度由 ClearType 三通道平均而来，与原生灰度 AA 不完全等价

---

## 7. 明确不做的事

| 不做 | 原因 |
|-----|------|
| ❌ 反射系统（RTTI / 按字符串设属性） | vczh 用十年换来的教训；无 XML 时性价比极低 |
| ❌ DependencyProperty 及优先级链 | 只保留一层 override，见 D4 |
| ❌ XML / 自造脚本语言 / 资源编译器 | 你已明确排除；GacGen 9 趟编译是复杂度黑洞 |
| ❌ 即时模式 API | 已排除 |
| ❌ 通用 RHI（Dawn/wgpu/bgfx） | UI 对 RHI 需求很窄，自研薄抽象 + D3D12 特化更优 |
| ❌ 第一版的 Linux / macOS / 移动端 | D11 |
| ❌ MSDF 字体 / subpixel AA | CJK 字形量太大；灰度 AA 与现代趋势一致 |
| ❌ 跨进程渲染分离（GacUI 有） | 第一版不需要 |

---

## 8. 仍然开放的问题

| # | 问题 | 影响 | 建议 |
|---|------|------|------|
| **Q7** | **扩展模型：组合优先 vs 继承优先** | D1 / D2 / D9 全部依赖它 | 推荐「组合优先 + 保留 C++ 子类化」 |
| Q8 | 场景再收窄到什么程度？ | GacUI 十年教训的核心 | ⚠️ **最该现在解决的问题**，见 §9 |
| Q9 | C++ 用户能否接受只有 fluent builder（无宏 DSL）？ | 影响 D1 的说服力 | 建议 M0 用两种语言各写一遍原型终结争论 |
| Q10 | 团队规模与时间预算？ | 决定 M3–M6 要不要砍 | —— |

---

## 9. 最大风险与止损点

### 9.1 风险排序

| 风险 | 说明 | 缓解 |
|-----|------|------|
| 🔴 **范围** | GacUI 实证：顶尖工程师 + 10 年 + 无采用。UI 库 80% 工作量在控件库/文本/平台集成，不在渲染内核 | **收窄场景**；控件库优先于渲染优化 |
| 🔴 **CPU 才是瓶颈** | 典型帧：布局 20% + 文本测量 30% + 命令生成 25% + GPU 光栅化 25%。GPU 只优化最后 25% | 增量布局 + 文本缓存优先 |
| 🟡 **两路径共存的复杂度** | base/override + token 是新引入的规则，测试矩阵翻倍 | M0 第 4 条直接验证 |
| 🟡 **跨语言 DSL 无法真正统一** | 最终会得到 5 套「各自还行但不同」的 DSL | 定义 **DSL 语义规范**（非语法），用 parity test 强制 |
| 🟡 **C++ 重建的分配成本** | Dart 重建便宜是因为 GC；C++ 是 malloc/free churn | arena + 对象池 + 只在 state 变化时重建 —— **架构前提，非后期优化** |

### 9.2 止损点（决策门）

| 门 | 时间点 | 如果…… | 那么…… |
|---|-------|--------|--------|
| **G0** | M0 结束 | 第 4 条（两路径共存）做不到自洽 | ⚠️ **架构重置**：退回纯重建模型（放弃直写快路径），重新评估 |
| **G0** | M0 结束 | 第 5 条（非 C++ 语言驱动）FFI 开销不可接受 | 重新设计 ABI 为批量提交协议，再验一轮 |
| **G1** | M2 结束 | 控件库工时 > 内核工时的 3 倍 | 场景必须再收窄，或砍掉 dock/节点图 |
| **G2** | M4 结束 | 团队外无人尝试绑定 | 「宿主语言原生 DSL」这个赌注错了，重新评估 |
| **G3** | 任意时点 | 单帧 CPU 侧（布局+文本+命令生成）> 8ms 且优化不动 | GPU 加速的收益被 CPU 吃掉，重新评估整体方向 |

**G0 必须在 M0 后立刻执行。** 越晚发现架构赌注错了，代价越大。

### 9.3 本方案相对 GacUI 的结构性优势

省掉**反射 + XML + 脚本语言 + 资源编译器**这四个子系统 —— 这不是省 10%，按 GacUI 的规模估算可能是**一半工程量**。代价是必须自己写 diff 引擎（约 2–3 人月）。

**2–3 人月换掉四个子系统，这是整个方案里最划算的一笔交易。**

---

## 10. 附录：绑定工具链调研（含对 D7 的修正）

### 10.1 直接回答：没有一个工具能一步到位

**成熟的「一份 IDL → N 种语言」生成器，全部是 Rust 源码导向的：**

| 工具 | 输入 | 输出语言 | 能否用于 C++ core |
|-----|------|---------|-----------------|
| **Diplomat**（Google） | **Rust 源码**（宏标注） | C, C++, Swift, WASM/JS, Dart | ❌ 源是 Rust |
| **UniFFI**（Mozilla） | Rust（UDL 或 proc-macro） | Kotlin, Swift, Python, Ruby（+ 社区 C#/Go/Dart/Node） | ❌ 源是 Rust |
| **SWIG** | C++ 头 + **手写 .i 接口文件** | 极广（20+） | ⚠️ 能用，但对现代 C++ 支持差、生成代码笨重、输出不地道、错误信息难懂 |

**你想找的那个「一个 IDL 喂进去、六种语言吐出来」的工具，对 C++ core 不存在。**

### 10.2 但你其实不太需要它 —— C ABI 本身就是半个代码生成器

关键洞察：**如果你把边界定成一个扁平的 C ABI，大多数语言根本不需要代码生成。**

| 语言 | 方式 | 工具 | 自动化程度 |
|-----|------|------|-----------|
| **Swift** | 原生 import C header | module map | ✅ **零代码生成** |
| **Kotlin / Java** | JNA / JNR | 无需生成 | ✅ **零代码生成** |
| **Python** | ctypes / cffi (ABI mode) | `headerkit` · `cffi` | ✅ 全自动 |
| **C#** | P/Invoke | **ClangSharp**（dotnet 官方） | ✅ 全自动 |
| **Rust** | `extern "C"` | `bindgen` | ✅ 全自动 |
| **TS / JS** | WASM | Emscripten `--emit-tsd` | ✅ 全自动 |
| **C++** | 直接 include | —— | 不需要 |
| Go | cgo | `c-for-go` | ✅ 全自动 |

**所以 D7 里「C ABI 是唯一边界」这个决定，本身就已经买下了 80% 的代码生成故事。** 剩下的 20% 是每个语言各跑一个现成工具。

### 10.3 控制平面 / 数据平面拆开

这是本附录最有用的建议 —— **两个平面用完全不同的方案**：

| | 控制平面 | 数据平面 |
|---|---------|---------|
| **传什么** | 创建节点、注册回调、改 state、直写属性 | 一次提交整棵 VNode 子树 |
| **频率** | 低（事件驱动） | 每次 reconcile |
| **方案** | 扁平 C ABI + 每语言 FFI 生成 | **FlatBuffers** |
| **代码生成** | 各语言各跑一个工具（§10.2） | **官方就有完整多语言 codegen** |

**FlatBuffers 对 VNode 提交是理想匹配**：零拷贝（正合跨 FFI 的那一跳）、嵌套表 + 向量天然表达树、且 [官方支持 C++/Rust/C#/Python/Java/TS/Go/Swift/Dart/Kotlin/Objective-C 等](https://flatbuffers.dev/)。

这一下把「数据平面要自己设计二进制编码 + 写 6 个语言的编解码器」这件事**整个消掉了**。

### 10.4 ⚠️ 对 D7 的修正：IDL 先别做

我在 §2 的 D7 里写了「IDL 驱动代码生成」。看完工具生态后**建议修正**：

> **不要一开始就做独立 IDL。用 C 头文件本身作为单一真源，IDL 推迟。**

理由：
- C ABI 本身就是一种被所有工具理解的 IDL —— 生态里每个生成器都读 C header
- 独立 IDL = 多一个解析器 + 多一个构建步骤 + **一份必须双向同步的真源**。这是净负担
- Rust 生态之所以造 uniffi/diplomat，是因为 **Rust 的 ABI 故事差**。C 的 ABI 故事本来就简单，你不需要那层包装

**C 头文件表达不了的东西**（谁负责 free、句柄借用还是拥有、回调必须在哪个线程、可能返回哪些错误码）——用 **SAL 风格的注解**（`_In_` / `_Out_` / `_Frees_ptr_` / `_Outptr_`）写在头文件里：

- ✅ 微软静态分析工具本来就懂，白拿一层检查
- ✅ 生成器（ClangSharp 等）能读
- ✅ 兼作文档
- ✅ **不引入第二种文件格式**

**什么时候才该引入 IDL：** 当你有 **≥3 种语言**、且头文件注解已经明显不够用时。届时 IDL 应该**生成头文件**（单向），而不是与头文件并存 —— 否则你就有两份真源。

### 10.5 🔴 坏消息：地道 DSL 层没有任何工具能生成

**§10.2 的流水线只解决 FFI 管道，不解决人机工效。**

下面这一层，**没有工具能做，只能手写**：

```rust
view! {
    VStack(spacing: 8) {
        Text(format!("count = {}", count.get())).font_size(24)
        Button("+1").on_click(move |_| count.set(count.get() + 1))
    }
    .padding(16)
}
```

- 没有工具能生成 Rust 的 `view!` proc-macro
- 没有工具能生成 Python 的 context manager DSL
- 没有工具能生成 C# 的 collection-initializer DSL

而且 **这是 D10「路线 C」的主要人工成本**。真实估算：

| 语言 | FFI 管道（自动） | 地道 DSL 层（手写） |
|-----|----------------|------------------|
| Rust | 1 天（bindgen） | **2-4 周**（proc-macro） |
| Python | 1 天（cffi） | **1-3 周**（context manager） |
| C# | 1 天（ClangSharp） | **2-4 周**（source generator） |
| Swift | 0（原生 import） | **2-4 周**（result builder） |
| TS | 1 天（Emscripten） | **2-4 周**（JSX 风格） |
| C++ | 0 | **1-2 周**（fluent builder，无宏） |

**每语言约 1 个月，且这是纯手工、无法自动化的工作。** 这就是 §11.5「跨语言 DSL 无法真正统一」在工程量上的具体体现。

**缓解手段只有一条：DSL 语义规范 + `tests/dsl_parity/` golden test**，保证各语言产出完全相同的 VNode 树。它不减少工作量，但能防止 6 套 DSL 语义漂移。

### 10.6 结论

| 问题 | 答案 |
|-----|------|
| 有工具能一步生成多语言绑定吗？ | ❌ 没有（成熟的都是 Rust 源导向） |
| 需要这个工具吗？ | ❌ 不需要 —— C ABI 本身就是半个生成器 |
| FFI 管道能自动化吗？ | ✅ 能，每语言 1 天，跑现成工具 |
| 数据平面能自动化吗？ | ✅ 能，用 FlatBuffers，全语言 codegen 官方自带 |
| 地道 DSL 能自动化吗？ | 🔴 **不能**，每语言约 1 个月手写，这是路线 C 的真实成本 |
| 要自建 IDL 吗？ | ❌ 先不要。用 C header + SAL 注解，≥3 语言后再评估 |
