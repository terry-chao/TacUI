# 写一个 UI

一个 include，就是全部公开面：

```cpp
#include <tacui/tacui.hpp>
```

`include/tacui/` 下的东西都是公开的。`core/`、`rhi/`、`platform/`、`tools/`
是**实现**，应用代码不应该伸手进去 —— 那样内部一改就会编译不过。

---

## 一棵树，一次提交

UI 用宿主语言构建一棵不可变的 VNode 描述，交给 core 对账。C++ 侧用 fluent builder：

```cpp
ui::VNode* buildUi(ui::Builder& b, App& app) {
    const int selected = static_cast<int>(app.ui->state(kSlotSelected, 0));

    auto root = b.stack();

    b.text("Scene Browser").box(20, 20, 400, 28).size(19).color(kTextStrong);
    b.rect().box(20, 68, 760, 1).color(kDivider);

    for (int i = 0; i < kItemCount; ++i) {
        const float y   = rowTop(i);
        const bool  sel = (i == selected);
        const bool  hot = (i == app.hoverRow);

        b.rect().box(kListX, y, kListW, kRowH).radius(8)
            .color(sel ? kRowSelected : (hot ? kRowHover : kRowPlain))
            .key(kRowBase + i);

        b.text(kItems[i].name).box(kListX + 36, y + 7, kListW - 48, 24)
            .size(16).color(sel ? kTextStrong : kTextDim);
    }

    return b.root();
}
```

可运行的完整源码：[`examples/asset_browser/main.cpp`](https://github.com/terry-chao/TacUI/blob/main/examples/asset_browser/main.cpp)。

`key` 是**稳定身份**。diff 靠它在前后两棵树之间对上号，直写通道也靠它找到目标节点。

---

## 输入是纯 C++

输入没有藏在绑定层后面，就是一个事件回调：

```cpp
auto onEvent = [&app](const host::Event& ev) {
    if (ev.type != host::EventType::MouseDown) return;
    for (int i = 0; i < kItemCount; ++i) {
        if (Rect{ kListX, rowTop(i), kListW, kRowH }.contains(ev.x, ev.y)) {
            app.ui->setState(kSlotSelected, i);   // 排一次重建
            return;
        }
    }
};
```

改 state 只是把树标脏；真正的重建发生在下一帧、在 core 内部。

---

## 两条路，按需要选

半静态的东西走声明式（改 state → 重建 → diff）；
每帧都在变的东西走直写（写 override → 只重绘）：

```cpp
// 拿一次，跨每次重建一直持有：
app.scan = app.ui->find(kScanFill).beginOverride();

// 每帧。从不标脏树，从不触发重建。
app.scan.setTranslate(phase * kScanSpan, 0.0f);
```

完整解释与优先级规则见 [两条变更通道](two-paths.md)。

---

## 谁拥有窗口和帧循环

两种用法：

| 用法 | 说明 |
|------|------|
| **`host::run`** | 现成的 Win32 + D3D12 外壳，示例与工具走这条；窗口、设备、帧循环都由它管 |
| **手动驱动** | 已经拥有窗口与渲染器的应用，自己调 `ui.update()` 与 `ui.paint()`，完全不碰 `host/` |

core 是可嵌入的：`host/` 只是便利路径，不是必需层。

---

## DPI：逻辑单位 + 物理渲染

进程声明 Per-Monitor V2 DPI 感知。你**始终用逻辑单位**写 UI（`host::Options`
的 `width`/`height` 也是逻辑单位），框架负责把它落到物理像素上：

```
窗口 / 后缓冲   width × scale          物理像素
hit test / 事件  坐标 ÷ scale            逻辑单位
布局             逻辑单位
字形             fontSize × scale       在物理像素上光栅化（不是放大)
paint            所有位置/尺寸/裁剪 × scale
```

关键的一条是**字形按物理尺寸光栅化**：150% 缩放下 14px 的字直接光栅成 21px 的位图
再 1:1 贴上屏幕，而不是把 14px 位图放大 1.5 倍。放大就会糊——那正是没有 DPI 感知时
Windows 对整个窗口做的事。

```cpp
host::Options opts;
opts.width  = 1280;   // 逻辑单位；150% 下窗口实际是 1920 物理像素
opts.height = 720;
// opts.uiScale = 0;  // 0 = 跟随显示器 DPI（默认）
```

`opts.uiScale = 1.0f` 让渲染与 DPI 无关——**截图/回归测试该用它**，否则同一份代码在
不同显示器上会产出不同像素。示例的 `--capture` 模式就是这么做的，所以仓库里的
基准图在任何 DPI 下都可复现。宿主语言走 C ABI 时用 `tui_set_ui_scale` / `ui.set_ui_scale`。

## 接着读

- [两条变更通道](two-paths.md) —— override、token 与生效值规则
- [布局](layout.md) —— row / column、尺寸提示与绝对子树平移
- [组件设计](components.md) —— 在写控件之前的输入系统与主题令牌
- [C ABI 与 Python 绑定](python.md) —— 同一个 UI 换个宿主语言
