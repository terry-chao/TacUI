# 控件库

> 状态：**基本控件已落地。** 12 个 L2 复合控件 + 一套主题令牌；
> `TextInput` 以「宿主持有缓冲」的形态先跑通输入与编辑。
> `ScrollView` / `ListView` 仍待内核支持裁剪，属于 L1 的下一批。

![控件总览](img/controls.png)

截图就是 `controls_gallery --capture` 的输出，可以直接当回归基准。

---

## 一句话规则

**组件是函数，不是类。**

```cpp
ui::button(b, kSave, { 20, 20, 96, 30 },
           { .label = "Save", .style = ui::ButtonStyle::Primary },
           [&app] { app.save(); });
```

这和 Flutter / SwiftUI 一致，也是跨 C ABI 唯一可行的形态 —— 类继承没法跨语言。
每个控件向 arena 里发射 VNode，并向输入系统登记自己的行为；**它自己不持有任何状态**。

由此得到两条构造上的保证：

* **视觉盒 = 命中盒**：两者都是同一个 `box` 参数，不可能漂移；
* **亮起来的区域 = 响应的区域**：hover / press / focus 从 `Ui` 读回，
  控件不做本地追踪。

---

## 组件清单

全部声明在 [`include/tacui/controls.hpp`](https://github.com/terry-chao/TacUI/blob/main/include/tacui/controls.hpp)，
实现在 `core/controls/controls.cpp`。

| 组件 | 样式 / 参数 | 交互 |
|------|------------|------|
| `label` | `size` · `color` · `strong` | 无（纯展示） |
| `button` | `ButtonStyle::{Primary, Secondary, Ghost, Danger}` · `enabled` | 点击 · Space / Enter · 焦点环 |
| `checkbox` | `checked` · `enabled` · `label` | 点击整行切换 · 键盘激活 |
| `radio` | `selected` · `enabled` | 点击选中 · 键盘激活 |
| `toggle` | `on` · `enabled`（开关样式） | 点击切换 |
| `slider` | `value` · `min` · `max` · `step` | **拖拽捕获** · 点击跳转 · 方向键微调 |
| `progressBar` | `value` 0–1 · `color` | 无（纯展示） |
| `divider` | `color` | 无 |
| `panel` | `fill` · `radius` · `border`，返回 `Scope` 容纳子节点 | 无 |
| `listItem` | `label` · `secondary` · `selected` | 点击选中 |
| `tabs` | `labels` · `count` · `selected`，键从 `firstKey + i` 起 | 点击切换 |
| `textInput` | `placeholder` · `suffix` · `maxLength` · `enabled` | 编辑 · 选区 · 光标 |

### TextInput 的编辑能力

光标是字节偏移，且永远落在码点边界上。支持：

| 按键 | 行为 |
|------|------|
| 可打印字符 | 插入（替换选区） |
| `Backspace` / `Delete` | 删除选区，或前 / 后一个**码点** |
| `←` / `→` | 移动光标；按住 Shift 扩展选区 |
| `Home` / `End` | 跳到行首 / 行尾 |

已知限制（都是 M0 文本栈的边界）：单行不换行、无横向滚动裁剪、光标不闪烁、
无 IME 组合、无剪贴板。

---

## 主题令牌

控件不硬编码颜色与尺寸，全部读 [`Theme`](https://github.com/terry-chao/TacUI/blob/main/include/tacui/theme.hpp)：

```cpp
struct Theme {
    Color background, surface, surfaceAlt, surfaceHover, surfaceActive;
    Color textStrong, textDim, textFaint, textOnAccent;
    Color border, borderStrong, accent, accentHover, accentSoft, danger, success, focusRing;
    float radiusSm, radiusMd, radiusLg, radiusPill;
    float spaceXs, spaceSm, spaceMd, spaceLg;
    float fontSm, fontMd, fontLg, fontXl;
};
```

内置 `darkTheme()` / `lightTheme()`，运行时换肤就是换一个结构体：

```cpp
ui.setTheme(ui::lightTheme());
```

令牌是在控件之前抽出来的（`components.md` 的 C4 决策）—— 事后再抽要改遍每个控件。
`darkTheme()` 复刻了示例原本硬编码的调色板，所以抽取没有改变原来的样子。

---

## 输入系统提供了什么

控件之所以能写得这么薄，是因为这些都在 core 里：

| 能力 | 谁负责 |
|------|--------|
| 命中测试（绘制顺序逆序、用 base bounds） | `Ui::hitTest` |
| hover / press / 失焦 | `Ui` 持有，`isHovered` / `isPressed` / `isFocused` 读回 |
| 焦点与 Tab 顺序 | `Ui::focusNext`，点击可聚焦节点夺焦 |
| 「按下后拖出去再松开不算点击」 | `Ui::dispatchEvent` |
| **拖拽捕获** | `NodeBehavior::onDrag`，按下时捕获，松开前一直收事件 |
| 键盘激活（Space / Enter） | 组件登记，输入系统分发 |

---

## 验证

```powershell
.\build\bin\controls_gallery.exe --input-test
```

**27 条断言，全部无窗口、无 GPU**，所以可以进 CI：

```
[PASS] disabled button does not fire
[PASS] slider takes the drag capture on press
[PASS] drag past the end clamps to max
[PASS] release ends the capture
[PASS] typing inserts characters
[PASS] backspace deletes a whole codepoint
[PASS] shift+arrow extends a selection
[PASS] typing replaces the selection
[PASS] Space activates the focused button
```

同一个可执行文件用 `--capture out.png` 出图，就是本页顶部那张。

---

## 下一步

| 顺序 | 内容 | 为什么 |
|---|---|---|
| 1 | **事件冒泡** | 嵌套交互（列表行里的按钮）需要它 |
| 2 | **裁剪** | `ScrollView` / `ListView` 的前提，需要渲染器支持 clip |
| 3 | **布局**（Measure / Arrange + BoxConstraints） | 控件现在靠调用方给绝对盒，M2 补齐 |
| 4 | 其余 L2 ：`Badge` `Toolbar` `Tabs` 变体 `Spinner` `Breadcrumb` | 体力活 |
| 5 | TextInput 升级为 L1：光标 / 选区搬到 Element，加 IME | 工程量大的一块 |

详见 [组件设计](components.md) §6。
