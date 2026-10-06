# 组件：放哪里，怎么设计

> 当前状态：**一个组件都没有。** `VType` 只有 Stack / Rect / Text 三种图元，
> 例子里的「按钮」是 Rect + Text + 手写命中测试拼出来的。
> 本文给出组件库的分层、归属和落地顺序。

---

## 1. 现状与缺口

例子里的按钮是这样实现的：

```cpp
// 建树
b.rect().box(kButtonX, kButtonY, 96, 30).radius(8).color(active ? kAccent : kPlain);
b.text("Bake").box(kButtonX + 30, kButtonY + 7, 80, 20).size(14);

// 命中测试 —— 手写
if (Rect{ kButtonX, kButtonY, 96, 30 }.contains(x, y)) { ... }

// hover —— 手写
const bool hot = app.hoverX >= 0 && b.contains(...);
if (hot != app.buttonHover) { app.buttonHover = hot; app.ui->invalidate(); }
```

**三个问题：**

1. 每个应用都要重写一遍 hover / press / 命中测试
2. 命中区域和视觉区域是两份数据，会漂移（改一处忘另一处）
3. 各应用行为不一致（tab 能不能聚焦？按空格算不算点击？）

**所以组件库的前提不是一个组件，而是 core 里的输入系统。**

---

## 2. 前提：core 需要输入系统 ✅ 已实现

现在 core 里没有：命中测试、事件路由、hover 追踪、焦点、拖拽捕获。全部由应用承担。

组件库需要它们全部。

### 已实现的形态

实现落在 `core/framework/ui.cpp`（命中测试与焦点遍历是文件内的自由函数；长大了再拆到 `core/input/`）。

```cpp
// 宿主把原始事件喂进来
ui.dispatchEvent(InputEvent{ ... });

// 组件在建树时读取交互状态
const bool hot     = b.ui().isHovered(key);
const bool pressed = b.ui().isPressed(key);
const bool focused = b.ui().isFocused(key);

// 组件在建树时声明自己做什么
b.behavior(key, ui::NodeBehavior{
    .onClick     = [&] { ... },
    .onKey       = [&](const InputEvent&) { ... },
    .interactive = true,   // 参与命中测试
    .focusable   = true,   // 参与 Tab 顺序
});
```

**关键：组件不持有自己的 hover/press 状态。** 输入系统持有它，组件通过 `Ui::isHovered/isPressed/isFocused` 读回。单一真源意味着「亮起来的区域」和「响应的区域」在构造上就不可能漂移 —— §1 问题 2 从根上消失了。

`NodeBehavior` 表在每次 rebuild 时清空重建；行为由 build 声明，不跨帧保留。

### 四个必须做对的地方

| 能力 | 实现 |
|-----|------|
| **命中测试** | 按**绘制顺序的逆序**（children 先于 parent，后面的兄弟先于前面的）。用 **base bounds**，不用 effective bounds —— override 是 paint-only，不能移动可交互区域（§14.4） |
| **hover 重建后重算** | 指针没动，但树可能动了，所以 `update()` 在 build **之前**重跑命中测试。这样 build 一次就拿到正确答案，不需要第二遍 |
| **捕获 / 拖出取消** | MouseDown 记下 `pressedKey_`；MouseUp 时只有命中同一个 key 才算点击。「按下后拖出去再松开」不算 —— 组件不必各自重写这段 |
| **焦点** | 属于 Element（跨帧稳定），按树序做 Tab 顺序；点击可聚焦节点夺焦（与桌面工具一致）。节点消失时才清焦点 |

**尚未实现：事件冒泡。** 现在命中测试直接返回最上层可交互节点，不向父节点回溯。做嵌套交互控件（List 行里的按钮）时需要补上。

**尚未实现：字符输入。** `KeyCode` 已有 `Character` 与 `codepoint` 字段，但 Win32 层还没接 `WM_CHAR`。这块随 TextInput 一起做。

---

## 3. 三层组件模型

```
┌─ L3  绑定层糖衣（各语言）─────────────────────────────────┐
│   ui.button("Save", key=K).on_click(...)                  │
│   薄封装，调用 L2 的 ABI                                     │
├─ L2  复合组件（core/controls/，经 ABI 暴露）───────────────┤
│   Button / Checkbox / Slider / Panel / Toolbar ...        │
│   纯函数：向 arena 里发射 VNode。不新增 VType                │
├─ L1  图元组件（core，新增 VType + RenderObject 状态）──────┤
│   TextInput / ScrollView / ListView / Popup               │
│   需要 core 侧状态、布局、输入处理                            │
├─ L0  图元（已有）─────────────────────────────────────────┤
│   Stack / Rect / Text                                      │
└───────────────────────────────────────────────────────────┘
```

**判据：一个组件属于 L1 还是 L2，只问一句话 ——**

> **它需要 core 侧的跨帧状态吗？**

- 不需要 → **L2 复合组件**。按两次状态变化重建一次 VNode 就够了。
- 需要 → **L1 图元**。比如 TextInput 的光标位置、选区、滚动偏移，都必须活在 Element 上。

按这个判据分：

| L2 复合（约 30 个，占绝大多数） | L1 图元（少数，但都是硬骨头） |
|---|---|
| Button · IconButton · ToggleButton | **TextInput** — 光标/选区/IME/横向滚动 |
| Label · Heading · Caption | **ScrollView** — 滚动偏移/滚轮/滚动条 |
| Checkbox · Radio · Switch | **ListView / Table / Tree** — 虚拟化，只实例化可见行 |
| Slider · ProgressBar · Spinner | **Popup / Menu / Tooltip** — 独立图层、焦点捕获 |
| Panel · Card · Divider · Badge | **Dock** — 拖拽停靠 |
| Toolbar · Tabs · Breadcrumb · StatusBar | |

**L2 大约占 30 个，L1 只有 5~6 个 —— 但 L1 的每一个都比 L2 全部加起来还重。**
TextInput 和 ListView 是整个控件库里工程量最大的两块，这一点和 GacUI 的经验一致（架构文档 §15.6）。

---

## 4. 每层放在哪

```
include/tacui/
    controls.hpp       L2 组件的公开声明
    input.hpp          命中测试 / 焦点的公开查询
    theme.hpp          设计令牌

core/input/            命中测试、路由、焦点、捕获
core/controls/         L2 组件实现 + L1 图元的 framework 部分
core/theme/            令牌表

capi/tacui.cpp         L2 组件经 ABI 暴露（每个组件一个函数）
bindings/*/            L3 糖衣
```

### L2 组件放 core，不放各语言绑定

这是一个要拍板的决定，取舍如下：

| | L2 放 core（推荐） | L2 放各语言 |
|---|---|---|
| 实现份数 | **1 份** | N 个语言 × 30 个组件 |
| 跨语言外观一致 | ✅ 天然一致 | ❌ 必然漂移 |
| 各语言地道感 | 🟡 靠 L3 糖衣补 | ✅ 天然地道 |
| ABI 面 | 每个组件一个函数（约 30 个） | 不增长 |
| 宿主自定义组件 | ✅ 仍可自己写 | ✅ |

**推荐放 core。** 理由是 N×M 的维护成本会失控，而且外观漂移会让「同一个界面换个语言写」这个卖点失效 —— 而那正是这个项目存在的理由。

L3 糖衣仍然要做（`ui.button(...)` 比 `tui_button(ui, key, x, y, w, h, ...)` 好读得多），但它变成薄薄一层。

**宿主仍然可以写自己的复合组件** —— 用 L3 的 builder 直接发射 VNode 就行，不需要 core 支持。core 提供的是「一套标准件」，不是「唯一的做法」。

### L2 组件的形态：函数，不是类

按 D9（组合优先 + 保留 C++ 子类化）：

```cpp
// core/controls/button.h
namespace tac::ui {

enum class ButtonStyle { Primary, Secondary, Ghost, Danger };

struct ButtonProps {
    const char* label   = "";
    ButtonStyle style   = ButtonStyle::Primary;
    bool        enabled = true;
};

// 发射 VNode，并把这些节点注册为可交互（交给输入系统）。
void button(Builder&            b,
            Key                key,
            Rect               box,
            const ButtonProps& props,
            ClickFn            onClick = {});
}
```

调用处：

```cpp
ui::button(b, kSave, { 20, 20, 96, 30 }, { .label = "Save" }, [&app] { app.save(); });
```

**组件是函数，不是类。** 这和 Flutter/SwiftUI 一致，也是跨语言能统一的唯一形态 —— 类继承没法跨 C ABI。

### 组件如何获得行为

组件不能只是图像，它得响应输入。机制是一张 **Key → 行为 的注册表**：

```cpp
struct NodeBehavior {
    ClickFn onClick;
    HoverFn onHover;
    bool    interactive = false;   // 参与命中测试
    bool    focusable   = false;   // 参与 Tab 顺序
    Cursor  cursor      = Cursor::Default;
};
```

建树时组件把自己的行为注册进去；输入系统命中测试后查表分发。这张表活在 Ui 上，每次 reconcile 后重建。

这样：
- 命中区域和视觉区域来自**同一个 `box`**，不可能漂移（解决 §1 的问题 2）
- hover / press / focus 的视觉反馈由组件自己写入 base 属性，走正常重建路径
- 点击回调是宿主提供的闭包

---

## 5. 主题令牌：让外观不漂移

如果 L2 放在 core，外观自然一致。但宿主自己写的组件仍然需要和标准件对得上。所以需要一层令牌：

```cpp
// include/tacui/theme.hpp
struct Theme {
    Color surface, surfaceAlt, surfaceHover;
    Color textStrong, textDim, textFaint;
    Color accent, accentHover, danger;
    float radiusSm, radiusMd, radiusLg;
    float spaceXs, spaceSm, spaceMd, spaceLg;
    float fontSm, fontMd, fontLg, fontXl;
};
```

例子现在把这些常量硬编码在 `main.cpp` 顶部（`kPanel`、`kRowHover` …）。它们应该来自 Theme，这样换肤就是换一个结构体。

---

## 6. 建议的落地顺序

| # | 做什么 | 状态 | 为什么这个顺序 |
|---|-------|------|--------------|
| **1** | **输入系统**（命中测试 / 焦点 / 捕获） | ✅ 已做 | 没有它，任何组件都只能像现在的例子一样手写 |
| **2** | **Theme 令牌** | ⬜ | 组件一写就会需要；事后抽取要改遍所有组件 |
| **3** | **Button + Label + Checkbox** | ⬜ | 三个最简单的 L2，用来验证整个模型是否成立 |
| **4** | **把例子改成用组件** | ⬜ | 例子是验收标准 |
| **5** | **TextInput（L1）** | ⬜ | 第一个真图元，验证「core 侧状态」这条路 |
| **6** | **ScrollView + ListView（L1）** | ⬜ | 虚拟化，游戏工具链的刚需 |
| 7 | 其余 L2 铺开 | ⬜ | 到这一步就是体力活了 |

### 第 1 步做完了，实际结果是：

例子已经**没有任何手写命中测试**（`contains(` 出现 0 次），也没有 `onEvent` 回调 —— 所有交互都由 build 声明。

但**行数几乎没变**（304 vs 300）。省掉的是命令式逻辑（hover 状态、MouseMove/MouseLeave/MouseDown 三个分支、边界框比较），换来的是声明。真正的价值不在行数，在两点：

1. **组件能封装它** —— 现在可以在 core 里写一个 `button()`，把「什么样」和「做什么」放在一起，使用方一行搞定
2. **能自动化验证** —— `asset_browser --input-test` 用合成事件跑 14 条断言，**headless，无窗口无 GPU**，可以进 CI

**第 3 步仍是分水岭。** 三个组件写完，就能知道这套分层对不对 —— 如果 Button 实现起来别扭，说明输入系统或 builder 的抽象有问题，此时推翻还便宜。

---

## 7. 要现在拍板的问题

| # | 问题 | 建议 |
|---|------|------|
| C1 | L2 组件放 core 还是各语言？ | **放 core**，理由见 §4 |
| C2 | 组件是函数还是类？ | **函数**（跨 C ABI 的唯一可行形态） |
| C3 | 输入系统做进 core 还是留给宿主？ | **进 core**，否则组件库不成立 |
| C4 | 主题令牌现在就抽，还是等组件多了再抽？ | **现在**，事后抽取要改遍所有组件 |
| C5 | 第一个 L1 图元选 TextInput 还是 ListView？ | **TextInput**，它单独就能解锁配置面板/脚本编辑器，而 ListView 依赖它的程度更低 |

---

## 8. 与其它文档的关系

- 分层依据：architecture.md §11.3（扩展模型）、§14（两条变更路径）
- 跨语言形态：plan.md D9（组合优先）、D10（core 提供 builder 基座）
- 输入与 override 的边界：architecture.md §14.4（命中测试用 base bounds）
- L1 图元的工程量参照：architecture.md §15.6（GacUI 的文本编辑器与虚拟化容器）
