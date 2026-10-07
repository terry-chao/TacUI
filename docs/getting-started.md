# 构建与运行

Windows、MSVC、CMake ≥ 3.25 与 Windows SDK。**核心没有第三方依赖** ——
构建它不需要 vcpkg，但把它当依赖用的时候可以走 vcpkg。

```sh
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
```

---

## 目标（targets）

| 目标 | 是什么 |
|------|--------|
| `asset_browser` | 示例：列表 + 选中 + 详情面板 + 动画指示条 —— **先跑这个** |
| `m0` | 验证 harness；`--selftest` 断言每一条 M0 验收标准 |
| `tacui.dll` | C ABI，给宿主语言用（`bindings/python`） |
| `textprobe` | 文本栈单独验证，不需要 GPU |

## 跑起来

```powershell
# 先看一个真实的界面
.\build\bin\asset_browser.exe

# 再把 M0 的六条验收标准跑一遍
.\build\bin\m0.exe --selftest
```

`asset_browser` 是同一个面板的两个版本：`examples/asset_browser/main.cpp`
与 `examples/asset_browser/browser.py`。两者布局、key、两条变更路径完全对应，
可以并排读；产出的是同一张图，只有宿主语言不同。

## 验收输出的读法

示例自己的数字，每秒打印一次：

```
rebuilds=1  paints=641  |  shaped=15 glyphs=78 quads=125  |  liveOverrides=1
```

641 帧画完，**只有 1 次重建** —— 也就是最初那一次。扫描指示条在这 641 帧里
一直在动，但控件树从未被碰过。换成一个 hover 行试试，`rebuilds` 会往上走，
因为 hover 改的是 **base** 颜色。

这一条就是 [两条变更通道](two-paths.md) 在数字上的样子。

## 用 Python 驱动

`tacui.dll` 构建好之后：

```powershell
cd bindings\python
python demo.py                # 交互；点蓝色按钮
python demo.py --selftest     # 脚本化断言，失败时退出码非 0
python demo.py --capture out.png
```

同一个界面也可以直接跑：

```powershell
cd examples\asset_browser
python browser.py
```

详见 [C ABI 与 Python 绑定](python.md)。

---

## 当依赖用

`cmake --install` 会留下头文件、二进制和一份 `TacUIConfig.cmake`，
所以它和别的 CMake 库一样能被 `find_package` 找到：

```cmake
find_package(TacUI CONFIG REQUIRED)
target_link_libraries(app PRIVATE TacUI::tacui_host)   # C++ 侧的全部
target_link_libraries(app PRIVATE TacUI::tacui)        # 只要 C ABI
```

`TacUI::tacui` 是 C ABI 单独那一个，**不带** `/EHsc`、`/permissive-`
这类 C++ 编译选项 —— C 宿主和非 C++ 宿主链接它不会有意外；
C++ 侧用 `TacUI::tacui_host`，它会把 `core / rhi / text / platform / tools`
整条依赖链一起带过来。

不想自己装就用仓库里的 overlay port：

```sh
vcpkg install tacui --overlay-ports=<仓库>/ports
```

该 port 覆盖 `x64-windows`、`x64-windows-static`、`x64-windows-static-md`
三个 triplet。静态 triplet 会把 `tacui` 从 DLL 变成静态库（C ABI 那个 target
也一样），因为「静态 CRT 的 DLL」并不存在。细节和提交流程见
[ports/README.md](https://github.com/terry-chao/TacUI/blob/main/ports/README.md)。

---

## 仓库里每个目录是什么

```
include/tacui/   公开 API —— tacui.hpp (C++) 与 tacui.h (C ABI)
core/            framework / renderer / text，无平台依赖
rhi/             后端无关接口 + D3D12 后端
platform/        窗口、输入（IME / DPI 后续补）
host/            现成的 Win32 + D3D12 外壳，给示例与工具用
capi/            C ABI 实现 —— 唯一的跨语言边界
bindings/        宿主语言封装；python/ 是已经被驱动过的那一个
examples/        asset_browser（C++ 与 Python 两版）
apps/            m0（验证 harness）、textprobe
ports/           vcpkg overlay port —— 让别处也能装它
cmake/           CMake 包文件（TacUIConfig.cmake.in）
```

`examples/` 与 `apps/` 只 include `<tacui/tacui.hpp>`。这条纪律由 CI 检查，
因为一旦内部头文件泄漏，重构内部结构就会破坏使用者。

## 接着读

- [写一个 UI](building-ui.md) —— 一个 include 就是全部公开面
- [两条变更通道](two-paths.md) —— 641 帧只重建一次是怎么做到的
- [架构](architecture.md) —— 为什么是这几层、为什么是 C ABI
