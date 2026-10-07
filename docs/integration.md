# 嵌入与对接

TacUI 的 core 是**可嵌入**的：应用自己拥有窗口和渲染器时，直接驱动
`ui.update()` 与 `ui.paint()` 即可，完全不碰 `host/`。

`host::run` 只是「还没写窗口与帧循环」时的便利路径。

---

## 嵌入的最小配方

只用公开 API：

```cpp
#include <tacui/tacui.hpp>

ui::Ui ui;
ui.init([](ui::BuildContext& ctx) {
    ui::Builder b(ctx);
    // ... 建树，或者直接调用 ui::button / ui::slider / ...
    return b.root();
});

// 宿主已经有的窗口句柄：Qt 的 winId()、MFC 的 m_hWnd、GLFW 的
// glfwGetWin32Window、SDL 的 SDL_PROP_WINDOW_WIN32_HWND_POINTER … 都行。
rhi::SwapchainDesc desc{};
desc.window      = hwnd;
desc.width       = width;
desc.height      = height;
desc.bufferCount = 2;

std::unique_ptr<rhi::Device> device = rhi::createD3D12Device(desc);

// 宿主的帧循环里：
ui.dispatchEvent(ui::InputEvent{ /* 平台事件翻译过来 */ });
ui.update();
device->beginFrame(ui.theme().background);
ui.paint(*device);
device->endFrame();
```

要点：

* **事件翻译是宿主的责任**：把平台的鼠标 / 键盘事件翻成 `ui::InputEvent`，
  核心永远看不见 `VK_*` 或 DOM key 字符串。
* **`ui.update()` 只在树脏时重建**，动画走 override 时不重建 —— 所以把它放在
  宿主循环里没有代价。
* **`host::run` 与嵌入二选一**。两者不冲突，但一个进程里通常只用一种。

---

## 对接 tamias

[tamias](https://github.com/terry-chao/tamias) 是 Qt 壳 + 自研 RHI 的桌面应用，
已经有一层界面抽象 `tac`（见其 `docs/TAC.md`）。对接有两条路，可以分阶段走：

### 路线 A：局部替换（推荐先做）

不动 Qt 壳，只用 TacUI 渲染 tamas 里**某一块面板或视口叠加层**：

1. 在 Qt 里拿一个子 widget 的 `winId()` 转成 `HWND`；
2. 按上一节的配方在该 `HWND` 上建 D3D12 swapchain，驱动 `ui.update()/paint()`；
3. tamias 的帧循环里调一次，其余部分照旧。

这条路的成本最低：不需要改 tamias 的界面层契约，也不需要 TacUI 支持
多窗口、弹层或 IME。适合先验证「同一份控件在 tamias 的暗色主题下好不好看」。

### 路线 B：实现 `tac` 后端

把 TacUI 实现成 tamias 界面抽象的一个后端，让上层代码不用改：

| tamias 的 `tac` 概念 | TacUI 上的对应物 |
|---|---|
| 窗口 / 画布 | `rhi::SwapchainDesc` + `host::run`，或宿主自驱 |
| 对话框 / 控件 | [`controls.hpp`](controls.md) 的 12 个控件 |
| 主题 | [`Theme`](controls.md) 令牌 |
| 事件 | `ui::InputEvent` + `ui.dispatchEvent` |

`tac` 的契约面比 TacUI 现在的公开面宽（对话框、文件选择、剪贴板等还没做），
所以路线 B 需要一侧补齐。**先用路线 A 拿到真实反馈，再决定补哪边**，
比一开始就对齐两套抽象便宜得多。

### 对接前 tamias 需要的东西

| 缺口 | 影响 | 什么时候需要 |
|------|------|------------|
| ~~事件冒泡~~ | 已完成：命中链 + 按消费传播 | —— |
| ~~裁剪~~ | 已完成：`Builder::clip()` + 图元 scissor | —— |
| ~~布局~~（row / column） | 已完成：相对容器按提示 / flex / 间距摆放，绝对子树整体平移 | —— |
| HarfBuzz + ICU | 现在的文本栈是单字体、码点→字形 1:1 | 中日韩排版、断行 |
| IME | 中文输入 | 任何需要输入中文的地方 |
| 多窗口 / 弹层 | 浮动面板、菜单 | 桌面应用形态 |

这些都是 [`plan.md`](plan.md) 里已经排过的 M1–M5 项，不是新增需求；
对接 tamias 的价值在于**给它们一个真实的验收场景**。

---

## 从 C ABI 对接

如果对接方不是 C++（tamias 的插件是 C#），走 `capi/` 的 C ABI：

* 构建与直写通道已经导出（代际句柄 + `tui_*`）；
* **控件层已导出**：`tui_label` / `tui_button` / `tui_checkbox` / `tui_radio` /
  `tui_toggle` / `tui_slider` / `tui_progress` / `tui_divider` / `tui_tabs` /
  `tui_panel`，宿主语言拿到的是和 C++ 同一套控件，不用自己重写 hover / 命中测试
  （见 [C ABI 与 Python 绑定](python.md)）；
* **输入注入与 `tui_update` 已导出**：`tui_dispatch_event` 把事件走
  `ui.dispatchEvent` 同一条路径送进框架，宿主可以自己驱动帧循环或做 headless 测试；
* **有状态控件也已导出**：`tui_text_input` / `tui_scroll_view` / `tui_list_view`，
  它们的跨帧状态由库按 key 托管，宿主用 `tui_text_get/set`、`tui_scroll_offset` 等访问器读写。
