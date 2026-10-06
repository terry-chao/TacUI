# TacUI

GPU 加速 · **retained mode** · C++ 核心 · 多语言**原生声明式** UI 库。

> 站点首页由模板渲染，见 [`overrides/home.html`](https://github.com/terry-chao/TacUI/blob/main/overrides/home.html)；
> 样式在 `docs/css/home.css`。这一页保留一份文字版说明，供搜索引擎与 GitHub 浏览用。

## 这个库的支点：两条变更通道

界面用**不可变描述 + diff** 来表达（Flutter 式三棵树），这是声明式的正确默认；
但每帧都在变的动画与外部数据如果也走重建，代价就白付了。

所以每个可动画属性都有两个通道，生效值只有一条规则：

```
路径 A · 声明式    改 state → 重建 VNode → diff → 写 base
路径 B · 直写      写 override → 只重绘，不重建

生效值 = override（若 token 活跃）否则 base
```

这不是折中，而是两类做法的交集：Flutter 的不可变描述 + diff，加上 WPF/WinUI 在实践中
被迫独立发明的直写快路径（`Translation` vs `Offset`），再明确拒绝 `DependencyProperty`
与反射。

## 这个库能做什么

| 能力 | 说明 | 文档 |
|---|---|---|
| **两条变更通道** | 声明式重建 + 直写 override；生效值 = `override ?: base`，没有第二套优先级 | [两条变更通道](two-paths.md) |
| **三棵树 + diff** | VNode / Element / RenderObject，稳定身份 + 不可变描述 + diff | [架构](architecture.md) |
| **C ABI 唯一边界** | 内部 struct 永不暴露，宿主只拿 `NodeRef`；代际句柄 + IDL 生成 | [C ABI 与 Python 绑定](python.md) |
| **Retained GPU 渲染** | 保留视觉树与 GPU 资源，属性变化只更新 uniform / transform | [架构](architecture.md) |
| **解析式反锯齿** | 矩形、圆角、文字各有解析覆盖路径；贝塞尔 AA 进 M1 | [方案](plan.md) |
| **原生声明式 DSL** | 各语言用自身惯用法表达同一棵树，用 parity test 保证语义一致 | [方案](plan.md) |
| **可嵌入的 core** | core 无宿主语言依赖；已有窗口与渲染器的应用自行驱动 `update()` / `paint()` | [写一个 UI](building-ui.md) |
| **组件与主题** | 先做 core 输入系统（命中 / 路由 / 焦点 / 捕获），组件放 core，主题走令牌 | [组件设计](components.md) |

## 先读这几篇定坐标

- [定位与目标](overview.md) —— 一句话定位、目标与非目标、当前状态
- [方案（决策清单）](plan.md) —— D1–D12 锁定决策、里程碑、开放问题与止损点
- [架构](architecture.md) —— 三棵树、渲染、绑定、DSL 统一问题、范式取舍
- [路线图与里程碑](roadmap.md) —— M0–M6 与 M0 的六条验收标准

## 跑起来

Windows、MSVC、CMake ≥ 3.25 与 Windows SDK，核心无第三方依赖：

```sh
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
```

先跑 `asset_browser`（列表示例），再用 `m0 --selftest` 验收 M0 的六条标准。
详见 [构建与运行](getting-started.md)。

## 仓库结构

```
include/tacui/   the public API — tacui.hpp (C++) and tacui.h (C ABI)
core/            framework, renderer, text    — no platform dependency
rhi/             backend-agnostic interface + the D3D12 backend
platform/        window, input, (later) IME and DPI
host/            a ready-made Win32 + D3D12 shell for examples and tools
capi/            the C ABI implementation — the only cross-language boundary
bindings/        host-language wrappers; python/ is the worked example
examples/        asset_browser (C++ and Python)
apps/            m0 (validation harness), textprobe
```
