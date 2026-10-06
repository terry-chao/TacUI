# 两条变更通道

这是整个设计的支点，也是它区别于「又一个声明式框架」的地方。

声明式重建是描述 UI 的正确默认；但它**不适配每帧都在变的东西**。所以每个可动画属性
都有两个通道，生效值只有一条规则：

```
路径 A · 声明式    改 state → 重建 VNode → diff → 写 base
路径 B · 直写      写 override → 只重绘，不重建

生效值 = override（若 token 活跃）否则 base
```

---

## 为什么要拆成两条

纯声明式框架里，一个每帧移动的动画会导致每帧重建整棵子树、diff、再写回布局。
这笔代价大部分白付了 —— 移动一个方块不该碰布局，更不该碰控件树。

直写通道把这类更新摘出去：它绕过 VNode、绕过 diff、绕过布局，只更新渲染属性并重绘。
**代价是引入了一个新概念（override + token），收益是高频变化不再有重建成本。**

这条设计不是凭空来的：WinUI 的 `Translation` vs `Offset` 就是微软被迫独立发明
base / override 分离的现实验证。TacUI 拿走这个结论，但**明确拒绝
`DependencyProperty` 与优先级链** —— 只保留一层 override。

---

## 一次租约，长期持有

动画在创建时拿一次 override 租约，之后一直往里写：

```cpp
// 拿一次，跨每次重建一直持有：
app.scan = app.ui->find(kScanFill).beginOverride();

// 每帧。从不标脏树，从不触发重建。
app.scan.setTranslate(phase * kScanSpan, 0.0f);
```

token 存活期间，这个属性的 override 压在 base 之上；token 释放，属性回落到 base。
reconcile 只会更新 base，**不会打断正在飞行的动画**。

---

## 在数字上的样子

示例自己的 stats，每秒打印一次：

```
rebuilds=1  paints=641  |  shaped=15 glyphs=78 quads=125  |  liveOverrides=1
```

641 帧画完，**只有 1 次重建**（最初的构建）。扫描指示条在这 641 帧里一直在走，
控件树却从未被碰过。把鼠标移到某一行上，`rebuilds` 开始涨 ——
因为 hover 改的是 **base** 颜色，命中测试与颜色都属于重建这条路。

框架里没有「这是动画控件」这类特判：决定权在**单个属性**上，不在整个控件上。

---

## 什么时候能用直写

直写只开放 **paint-only** 属性：

| 可以直写 | 必须走重建 |
|---------|-----------|
| translate / scale / rotate / opacity / color / paint 层 offset | 所有影响布局的属性（size / padding / alignment / 可见性） |

这条边界不是保守，是必需的：直写如果动了布局，布局缓存就被打乱，
增量布局的前提消失。**这条守不住，整个快路径要返工。**

---

## M0 是怎么证明它的

M0 的第 3、4 条验收标准直接盯着这条路：

- **第 3 条**：动画直写，`rebuilds << paints`，动画帧零重建
- **第 4 条**：两条路径作用在**同一个节点**上互不干扰 —— DSL 写 `.opacity(1.0)`，
  动画覆盖 `opacity`，动画生效；reconcile 不打断动画，动画结束后回落

证据与每一条的结果见 [路线图与里程碑](roadmap.md)。

## 接着读

- [架构](architecture.md) §14 —— 完整分层、数据结构与优先级规则
- [方案](plan.md) D4 —— base / override 双通道的决策记录
- [写一个 UI](building-ui.md) —— 代码里的两条路径
