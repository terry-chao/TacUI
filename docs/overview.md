# 定位与目标

> **一句话定位：一个 GPU 加速、retained mode、C++ 核心、可在任意宿主语言里用原生声明式语法编写 UI 的界面库。**

结构上参考 GacUI 的「逻辑与表现分离 + 每元素持 GPU 资源」，但**砍掉 GacUI 的反射系统、
XML 资源和 Workflow 脚本**，改由宿主语言自身承担声明式表达。

---

## 目标

| # | 目标 | 说明 |
|---|------|------|
| G1 | GPU 加速渲染 | 光栅化、合成、动画插值全部走 GPU |
| G2 | Retained mode | 保留视觉树 + 保留 GPU 资源，属性变化只更新 uniform / transform，不重建 GPU 对象 |
| G3 | C++ 核心 | 无宿主语言依赖的独立 core，可嵌入任何进程 |
| G4 | 多语言绑定 | Rust / C# / Python / Swift / Kotlin / TS 等，由稳定的 C ABI + IDL 代码生成 |
| G5 | 代码式声明 UI | 无 XML、无独立脚本语言，各宿主语言用其原生语法表达 UI |
| G6 | 可嵌入 | 能作为库嵌进已有应用（游戏引擎、桌面 App、插件宿主） |

## 明确非目标（第一版）

- 不做即时模式（immediate mode）API
- 不做跨平台移动端（Windows 优先，Linux / macOS 次之）
- 不做 HTML / CSS 兼容层
- 不追求 100% 像素级还原任何现有框架

---

## 两条变更通道

这是整个设计的支点，也是它区别于「又一个声明式框架」的地方。

```
路径 A · 声明式    改 state → 重建 VNode → diff → 更新 base → 必要时重排 / 重绘
路径 B · 直写      写 override（animation / 命令式）→ 只重绘，零重建

生效值 = override（若 token 活跃）否则 base
```

声明式重建是描述 UI 的正确默认；但它不适配每帧都在变的东西。直写通道把动画和外部数据
推送从重建里摘出去，而且没有引入第二套优先级规则 —— 决定权落在**单个属性**上，
框架里不存在「这是动画控件」这类特判。

直写只开放 **paint-only** 属性（translate / scale / rotate / opacity / color）；
所有影响布局的属性必须走重建。这条守不住，整个快路径要返工。

详见 [两条变更通道](two-paths.md)。

---

## 当前状态

**M0（架构验证）已完成。** 一个 GPU 渲染、retained-mode 的 UI，C++ 核心 + 一个能被
非 C++ 语言驱动的 C ABI。RHI 目前只有 D3D12；控件库、布局引擎与文本栈都还很薄。

已建成的基建：

| 基建 | 作用 |
|------|------|
| `--selftest`（C++ 与 Python 各一份） | 把 M0 的验收标准变成自动断言 |
| 帧回读 → PNG | golden-image 测试的种子 |
| D3D12 校验层诊断通道 | 捕获 API 误用 |
| `textprobe` | 不依赖 GPU 即可验证文本栈 |
| `tools/inspector`（规划） | 视觉树 / 帧分析 / override 可视化 |

准确的能力边界见 [方案 §6.1](plan.md) 与 [路线图](roadmap.md)。

---

## 相关文档

- [方案（决策清单）](plan.md) —— D1–D12、里程碑、开放问题、止损点
- [架构](architecture.md) —— 三棵树、渲染、绑定、DSL、WPF vs Flutter
- [组件设计](components.md) —— 组件分层、输入系统、主题令牌
- [C ABI 与 Python 绑定](python.md) —— 唯一跨语言边界长什么样
