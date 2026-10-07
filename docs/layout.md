# 布局

> 状态：**相对容器（row / column）已落地。** 容器按自身盒分配空间，
> 子节点用尺寸提示、`flex`、`gap`、`margin`、对齐来摆放；绝对定位子树整体平移，
> 因此既有的「每个节点给一个绝对盒」写法完全不受影响。
> 仍待补：Measure/Arrange 的通用约束传播（跨轴 intrinsic、自动换行、`Popup` 的独立图层）。

控件此前都靠调用方给绝对盒 —— 位置、行高、内容高度都得自己算。
这一步把「谁在哪儿」交给容器，调用方只需要说「这一列从上到下、间距 8」。

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
本版的布局是**帧式**的：容器拿到一个明确的盒，再把它分给子节点；
`BoxConstraints` 就是这次分配使用的语言。

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
| 帧式而非约束式 | 容器必须有明确盒；还没有「父给约束、子回报尺寸」的两遍 Measure/Arrange | 自动换行、按内容撑开窗口 |
| 无自动换行 / 网格 | 只有单行、单列 | `WrapPanel`、`Grid` 这类容器 |
| 交叉轴 intrinsic 是单遍 | 子节点交叉轴尺寸在摆放前测量一次，不会因主轴分配二次回测 | 交叉轴依赖主轴尺寸的子节点 |
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
```
