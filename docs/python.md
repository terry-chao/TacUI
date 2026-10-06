# C ABI 与 Python 绑定

跨语言只有**一个**边界：C ABI。内部 struct 永远不暴露出去，
宿主语言拿到的是 `NodeRef` —— 所以内部实现从 AoS 换成 SoA、把渲染搬到独立线程，
对绑定都是 drop-in 的。

---

## C ABI 的形状

句柄是**代际句柄**：`{ index, generation }` 两个 32 位整数。节点被销毁后，
旧句柄解析为 null，而不是指向另一个节点 —— 这就是 index + generation 这对组合的意义。

```c
// capi/ 导出面的形状（IDL 生成的目标）
tui_node_ref_t node = tui_scene_get_ref(scene, "marker_42");
tui_write_begin(node);
tui_write_f32(node, TUI_PROP_TRANSLATE_X, 120.0f);
tui_write_f32(node, TUI_PROP_TRANSLATE_Y,  88.0f);
tui_write_f32(node, TUI_PROP_OPACITY,       0.6f);
tui_write_commit(node);       // 离开作用域 → revoke
```

第一版**先不做 IDL**：让 C header 自己当那半个代码生成器，等第二个宿主语言落地
再抽共用的生成器。理由见 [方案](plan.md) §10.4。

ABA 版本通过 `TUI_ABI_VERSION` 协商；`tui_stats_size()` 让绑定在 import 时
比对结构体大小，把「ctypes 不校验大小 → 静默写穿缓冲区」这类内存破坏变成加载期报错。

---

## Python 侧的分工

`bindings/python/tacui.py` 是**手写**的 ctypes 绑定 —— 故意的。按 [方案](plan.md) §10，
FFI 管道是机械的部分；真正每个宿主语言都要自己写的是**地道的那一层**，
这个文件就是 Python 形状的它。

它演示的分工很干净：

```
C++   拥有窗口、GPU 设备、帧循环          (tui_run)
Python 拥有 UI：build 回调、事件处理、动画
```

![Python 宿主驱动的 TacUI 窗口](img/app-shot-python.png)

---

## 跑一下

先构建出 `tacui.dll`（`capi/` 目标），然后：

```powershell
cd bindings\python
python demo.py                # 交互；点蓝色按钮，Esc 退出
python demo.py --selftest     # 脚本化断言，失败时退出码非 0
python demo.py --capture out.png
```

`demo.py` 不是「Hello World」：它跨 ABI 验证了 M0 的三条关键标准 ——

```
[PASS] criterion 3: painted nearly every frame
[PASS] criterion 2: host-driven setState caused rebuilds
[PASS] criterion 3: animation frames did not rebuild (rebuilds << paints)
[PASS] criterion 4: override survived repeated rebuilds
[PASS] criterion 4: coexist override still installed at exit
[PASS] retention: elements were reused across rebuilds, none destroyed
[PASS] criterion 6: tree dump produced nodes
```

同一份 core，同一个面板：`examples/asset_browser/browser.py` 与
`examples/asset_browser/main.cpp` 并排可读，布局、key、两条路径一一对应。

---

## 写第二个宿主语言时暴露的 ABI 问题

这部分反馈比测试更有价值 —— 都是**只有换语言才会暴露**的问题：

- 代际句柄在 ctypes 下工作良好，`__bool__` 语义自然
- `user` 指针 + 宿主侧注册表是可行模式（比闭包更贴合 ABI 意图）
- **构建面确实 chatty**：每节点 3–4 次 FFI。可接受，但大树的批量提交迟早要做
- **ctypes 回调里抛异常不会传播进 C 循环** —— 宿主的错误会静默丢失，
  ABI 需要一条回传错误的路径（M1）
- 首帧回调曾经跑在树建立之前，宿主的句柄解析必然失败；`tui_run` 已统一为 UTF-8 字符串，
  并修正了这个时序

## 接着读

- [方案](plan.md) §5 与 §10 —— 边界设计原则与绑定工具链调研
- [架构](architecture.md) §5 —— 跨语言绑定
- [写一个 UI](building-ui.md) —— C++ 侧的同一套 API
