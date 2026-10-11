# 布局

> 状态：**两遍 measure / arrange 已落地。** 容器先按约束测量子节点，再把自己
> 决定的槽位安排下去：`flex` 分主轴、`gap` / `margin` 定间距、`align` 定交叉轴；
> 框上写 `kAuto` 的轴按内容撑开，给不出宽度时文本自动换行。绝对定位子树整体平移，
> 因此既有的「每个节点给一个绝对盒」写法完全不受影响。
> 仍待补：`WrapPanel` / `Grid` 这类容器、`Popup` 的独立图层。

控件此前都靠调用方给绝对盒 —— 位置、行高、内容高度都得自己算。
这一步把「谁在哪儿」交给容器，调用方只需要说「这一列从上到下、间距 8」。
再进一步：容器也可以**不给自己盒**，让它长成内容的样子。

---

## 值类型：`BoxConstraints`

布局的契约是**值类型**，不是虚函数（plan.md D6）。这是刻意的：值类型能过 C ABI，
`MeasureOverride` 那种虚函数不行。

```cpp
#include <tacui/tacui.hpp>   // 含 layout.hpp

ui::Size        s{ 120.0f, 32.0f };
ui::EdgeInsets  pad = ui::EdgeInsets::symmetric(12.0f, 8.0f);
ui::BoxConstraints c = ui::BoxConstraints::loose(200.0f, 100.0f);
```

`kAuto`（`-1`）表示「不指定，用 intrinsic 尺寸」，和 `0`（零尺寸盒）是两回事。

布局是**两遍**的（[方案](plan.md) D6）：

```
measure(child, 约束) → 想要的尺寸      自下而上：父节点给什么，决定子节点报多大
arrange(child, 槽位) → 把子树放进去     自上而下：父节点定的槽位，就是子节点最终的位置
```

`BoxConstraints` 就是这两遍之间说的语言。它是纯值类型，不是 `MeasureOverride`
那样的虚函数 —— 值类型能过 C ABI，虚函数不能，这是整个 D6 的前提。

对容器来说，两遍共用同一份分配逻辑（`planContainer`）：`measure` 按父节点给的约束
求解，`arrange` 按自己实际拿到的盒求解，同一个盒得到同一个答案。

---

## 相对容器：`row` / `column`

```cpp
auto root = b.stack();

// 一列：从上到下，间距 10，内边距 10。
auto col = b.column({ 0, 0, 400, 300 },
                    { .gap = 10.0f, .padding = ui::EdgeInsets::all(10.0f) },
                    kColumn);

b.rect().width(100).height(50).key(kA);           // 第 1 行
b.rect().width(100).height(50).key(kB);           // 第 2 行
b.rect().height(30).flex(1.0f).key(kC);           // 吃掉剩下的高度
```

`column` 把 `{0,0,400,300}` 当作框架：子节点**不再需要自己的 x/y**，
容器决定。`gap` 是相邻子节点之间的间距，`padding` 在分配前内缩框架。

子节点尺寸的规则：

| 提示 | 含义 |
|------|------|
| 不给 | 用 intrinsic 尺寸（文本按测量宽度，矩形按它在树里的宽高） |
| `.width(w)` / `.height(h)` | 钉住某一轴 |
| `.flex(f)` | 与兄弟按权重瓜分主轴的剩余空间 |
| `.margin(...)` | 在槽位内再内缩 |
| `.align(x, y)` | 在交叉轴上摆放（0 = 起点，0.5 = 居中，1 = 终点） |

```cpp
auto row = b.row({ 0, 0, 300, 100 }, { .gap = 8.0f }, kRow);
b.rect().width(100).height(40).key(kLeft);        // 固定宽度
b.rect().height(40).flex(1.0f).key(kFill);        // 占满剩余宽度
b.rect().width(40).height(40).align(0.5f, 1.0f)   // 交叉轴居中、贴底
    .key(kCorner);
```

---

## 绝对子树整体平移

相对容器里也可以放**绝对定位**的东西 —— 一个 `panel`、一个 `clip`、一个裸 `stack`。
布局不重排它的内部，而是把整棵子树平移到槽位：

```cpp
auto row = b.row({ 0, 0, 600, 200 }, { .gap = 12.0f });

{
    auto card = ui::panel(b, kCard, { 900, 900, 280, 180 });  // 写在哪都行
    ui::label(b, kTitle, { 916, 916, 200, 24 }, { .text = "Card" });
}
```

`panel` 内部仍然按绝对坐标排，但整个卡片会被移到行的槽位上 ——
「容器决定它落在哪，子节点决定它长什么样」。

字形位置按**节点局部坐标**缓存，所以平移一个文本节点不需要重新 shape。

---

## 按内容撑开：`kAuto` 的轴

框的某一轴写 `kAuto`（或非正数），那一轴就不再是「我要这么宽」，而是
「长成内容的样子」—— 这是「按内容撑开窗口」在容器这一层的形态。

```cpp
// 没有盒的列：宽度取最宽的孩子，高度是孩子们加间距的总和。
auto col = b.column({ 0, 0, ui::kAuto, ui::kAuto }, { .gap = 6.0f });
b.rect().width(80).height(20);
b.rect().width(40).height(30);
// → 列自己是 80 x 56，两个矩形分别落在 (0,0) 和 (0,26)
```

规则很直白：**轴上有数就钉住，没有数就量出来**。

| 框的轴 | 含义 | 谁决定尺寸 |
|--------|------|-----------|
| 正数（或 `.width(w)` 提示） | 钉住 | 调用方 |
| `kAuto` / `0` / 负数 | 撑开 | 内容（主轴 = 孩子之和 + 间距；交叉轴 = 最大的孩子） |

`measure` 时父节点给的约束仍然生效：父节点更紧就按父节点的来，孩子撑不破它。

---

## 自动换行

文本的测量**依赖约束**，这是两遍布局和帧式布局最实质的差别：

```cpp
auto col = b.column({ 0, 0, 120, ui::kAuto });   // 宽 120，高按内容
b.text("wrap me onto several lines please").size(16.0f);
```

文本拿到「宽最多 120」，就把自己排成多行，回报的尺寸是**换行之后**的
（例子里是 98.8 x 63.8，三行；不换行时宽 237.6）。于是下面的兄弟节点自动落在
换行后的高度之下 —— 不需要调用方知道行数。

断行规则是贪心的：

- 优先在**最后一个放得下的空格**（或 CJK 字符边界）断；
- 一个词本身就比行还宽时，词内断，不让它溢出；
- `\n` 强制断行；
- 空格被断行吃掉，不留在行尾。

文本块的缓存键是 `(文本, 字号, DPI, 换行宽度)`：布局阶段问的宽度和 reconcile
阶段问的不一样，键里带宽度才是缓存命中而不是每帧重排（见[框架实现](framework.md)）。

---

## 一个控件 = 一个槽

每个 L2 控件会把自己的节点收进一个容器，所以在相对容器里**只占一个槽**：

```cpp
auto row = b.row({ 0, 0, 600, 48 }, { .gap = 12.0f });
ui::button(b, kOk,     { 0, 0, 96, 32 }, { .label = "OK" });
ui::button(b, kCancel, { 0, 0, 96, 32 }, { .label = "Cancel" });
```

两个按钮按 12px 间距横排，`x` 由布局算出来。

---

## 已知限制

这些是**这一版刻意没做的事**，不是 bug：

| 限制 | 说明 | 何时需要 |
|------|------|---------|
| 控件不随槽位重排内部 | 相对容器里，控件保持**调用方给它的盒尺寸**，只换位置；`flex` 会拉伸槽位，但控件的绝对内部不会跟着重排 | 需要「按钮随窗口变宽」时，把控件做成相对容器 |
| 无自动换行 / 网格 | 只有单行、单列 | `WrapPanel`、`Grid` 这类容器 |
| 交叉轴只测一遍 | 子节点在摆放前测量一次，主轴被 `flex` 改动后不会回测交叉轴 —— 所以 `flex` 的文本节点不会按分配到的宽度重新换行 | 交叉轴依赖主轴尺寸的子节点 |
| L1 容器仍是绝对 | `scrollView` / `listView` 仍要显式视口盒（它们的滚动几何依赖它） | 滚动区域内嵌套相对布局 |

---

## 验证

```powershell
.\build\bin\controls_gallery.exe --layout-test
```

断言无窗口、无 GPU，可进 CI：

```
[PASS] column places the first child at the padded origin
[PASS] column advances by child height + gap
[PASS] flex child fills the leftover main-axis space
[PASS] row flex child takes exactly the remaining width
[PASS] align(0,1) pins the child to the bottom edge
[PASS] a panel authored far away lands at the row slot
[PASS] a bare text node takes its measured width
[PASS] hit testing uses the laid-out position, not the authored one
[PASS] an auto-framed column stacks from its own origin
[PASS] an auto-framed row advances by child width + gap
[PASS] wrapped text stays inside the width it was offered
[PASS] wrapped text is taller than a single line
[PASS] the sibling below moves by the wrapped height
```

断行算法本身在 `textprobe` 里另有一份不依赖窗口与 GPU 的断言：每行都不超宽、
字形一个不丢、块高由行数决定、无空格的超长 token 会被词内断（`textprobe` 与
`textprobe --cjk` 各跑一遍，后者走的是字符边界断行）。

## 接着读

- [框架实现](framework.md) —— 布局在 `update()` 里的位置、`translateSubtree` 为什么不用重新 shape
- [组件设计](components.md) §6 —— 通用约束传播（Measure/Arrange）为什么排在相对布局之后
