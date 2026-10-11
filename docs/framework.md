# 框架实现

> 状态：**M0/M2 已落地的部分** —— 三棵树、reconcile、override、帧式布局、输入路由。
> 对应实现：`include/tacui/{vnode,element,ui,input,layout}.hpp`、
> `core/framework/*.cpp`。**为什么**这么分层见 [架构 §2/§14](architecture.md)；
> 本文写**代码实际怎么运转、每条规则的边界在哪**。

---

## 三棵树，各自什么时候被碰

| 树 | 类型 | 生命周期 | 谁写 | 什么时候写 |
|----|------|----------|------|------------|
| VNode | 不可变描述 | 一次 rebuild | 宿主的 build 函数 | 树脏时 |
| Element | 可变、跨帧稳定 | 跨帧 | reconcile | 每次 rebuild |
| RenderObject | 可变、跨帧稳定 | 跨帧 | reconcile（base）+ 直写（override） | rebuild / 每帧 override |

`Ui::update()` 是唯一推进这棵树的地方，它只在 `dirty_` 时做真事：

```cpp
bool Ui::update() {
    if (!dirty_ || !build_) return false;   // ← early-return 有两个后果，见下
    ...
}
```

**这个 early-return 是两条变更通道的性能来源，也是一个坑的来源。**
它意味着「树不脏的一帧」里 update 什么统计都不更新。`liveOverrides` 曾经写在
update 里，于是永远停在第一次构建时的值；搬到 `paint()`（它本来就在遍历树）才对。
教训：**每帧都要准的计数，只能放在每帧都跑的函数里。**

---

## VNode：arena 分配，rebuild 零堆分配

VNode 树每次 rebuild 从头重建，但**不是每帧重建堆**。`VNodeArena` 是 bump 分配器：

```cpp
void VNodeArena::reset();   // O(1)：offset_ = 0，块全部留着
```

稳态下一棵大小相似的树，rebuild **一次堆分配都不发生** —— `bytesAllocated()` /
`blockCount()` 可以直接断言这件事。这是对「C++ 重建 = malloc/free churn」
这条风险的直接回应（[架构 §13.3](architecture.md) / [方案](plan.md) §9.1）。

文本字符串也 `strdup` 进 arena，所以 VNode 永不持有调用方的指针；调用方的字符串
只要活过这次 build 就行。

---

## reconcile：一个节点，两条写路径

reconcile 是「用新 VNode 更新旧 Element」的地方，也是 base / override 分离的
**执行点**。规则只有两类：

```
reconcile(e, v):
  if e.type != v.type || e.key != v.key:
      → 这是另一个节点：整体替换
        · 用 v 挂载一棵新子树
        · e.generation++          （作废所有现存 OverrideToken）
        · e.states.clear()        （“换了个控件，状态重来”）
  else:
      → 同一个节点：只写 base
        · 更新 bounds / color / cornerRadius / layout 字段
        · 文本/字号/scale 变了才重新 shape
        · **绝不碰 override 通道**   ← M0 第 4 条标准就靠这一条
        · reconcileChildren（目前按位置匹配）
```

子节点目前**按位置匹配**：收缩就丢尾巴，增长就挂新的。按 key 的列表重排
（key matching / reorder）在 M2。

### 节点身份 = 路径 + key + 位置

M0 的 `findByKey` 是**深度优先全树扫描**，`OverrideToken` / `NodeRef` 用
`weak_ptr<Element> + generation` 表达。这是刻意的**临时实现**：M1 会换成
reconcile 期间维护的 id → element 映射，以及 C ABI 用的代际索引。
两者保证相同的语义 —— 旧句柄解析为 null，而不是指向别的节点。

---

## Override：一层，一个 token，一条规则

生效值的**全部**优先级系统就是这一行：

```cpp
Rect  effectiveBounds(const RenderObject& ro) { return ro.bounds + ro.overrideTranslate; }
Color effectiveColor (const RenderObject& ro) { return ro.baseColor × overrideOpacity; }

生效值 = override（若 token 活跃）否则 base
```

三条硬性设计，全部在代码里强制而非只写在文档里：

1. **每节点同时最多一个 override。** `NodeRef::beginOverride()` 发现已经有活跃
   override 就返回**无效 token 并打警告**，而不是排队或覆盖。这是「为什么只有
   一层优先级、不需要 DependencyProperty」的落地。
2. **token 跨节点替换自动失效。** token 记 `(weak_ptr, generation)`；
   `lockedTarget()` 发现 `e->generation != generation_` 时返回 null，
   于是**写一个已经被替换掉的节点会静默失败**，而不是去驱动新节点的 RenderObject。
   （这个 bug 真实发生过一次。）
3. **直写只开放 paint-only 属性。** `OverrideToken` 只暴露 `setTranslate` /
   `setOpacity`。能直写的是 translate / opacity；`color` 由 opacity 乘进去实现
   （`effectiveColor`）。凡是影响布局的属性（size / padding / 对齐 / 可见性）
   没有 setter —— **不给 API 就是不给机会**，比写文档说「别这么干」可靠。

`release()` 把 override 字段复位回中性值（translate 0、opacity 1、active=false），
**不碰 base**，所以动画结束后节点自然回落到它该有的样子。

### 用户可见的三条推论

- `NodeRef::bounds()` 返回的是 **base bounds**；命中测试也永远用 base bounds ——
  override 是 paint-only，不能移动可交互区域。
- 一个飞行中的动画**不怕 reconcile**：reconcile 只写 base，动画只写 override，
  两者在同一个 RenderObject 上互不干扰（`reconcilesWithLiveOverride` 计数为证）。
- 动画 token 的正确持有方式是**拿一次、长期持有**（`app.scan = ...beginOverride()`），
  不是每帧重新拿。

---

## 布局：帧式，先 reconcile 后布局

布局是**帧式**的（[方案](plan.md) D6）：容器拿到一个明确的盒，再把它分给子节点。
`BoxConstraints` 是这次分配说的语言，是纯值类型（能过 C ABI，虚函数不能）。

### 顺序：reconcile → layout，不能反

```cpp
reconcile(*root_, *v, rs, ctx);   // 顺便 shape 文本
layoutTree(*root_, ctx);          // 需要 intrinsic 文本宽度，所以必须在后面
```

文本节点的内在尺寸要靠 shaping 结果，而 shaping 发生在 reconcile 里，
所以布局必须排在 reconcile 之后。

### 相对容器：row / column

`Stack` 带 `layout = Row | Column` 就成了相对容器。分配算法（`arrangeContainer`）：

1. 内缩 `padding` 得到内容区；
2. 每个孩子算 `intrinsicSize`（显式 hint 优先，否则内在尺寸）；
3. 固定孩子占用主轴总量，`flex > 0` 的孩子按权重分**剩余**主轴；
4. 主轴依次排列，间隔 `gap`；交叉轴按 `alignX/alignY`（0=起点，0.5=居中，1=终点）；
5. 孩子自己的 `margin` 在槽位内再内缩。

### 绝对子树整体平移（一个必须做对的地方）

相对容器里也可以放**绝对定位**的子树（panel / clip / 裸 stack）。布局不重排它内部，
而是**把整棵子树平移**到槽位：

```cpp
void translateSubtree(Element& e, float dx, float dy);  // 平移 paint bounds、clip、frame
```

字形坐标是**节点局部**的（见 [渲染管线](rendering.md)），所以平移一个文本节点
**不需要重新 shape** —— 只在 paint 时加最终原点。这就是「节点局部坐标」这条设计
在框架侧的回报。

### 已知限制（这一版故意没做）

| 限制 | 影响 | 何时需要 |
|------|------|----------|
| 帧式而非两遍约束 | 容器必须有明确盒 | 自动换行、按内容撑开窗口 |
| 只有单行/单列 | 无 `WrapPanel` / `Grid` | 网格、流式布局 |
| 控件不随槽位重排内部 | `flex` 拉伸槽位，但控件内部绝对坐标不动 | 按钮随窗口变宽 |
| 交叉轴 intrinsic 单遍 | 交叉轴依赖主轴的孩子要二次回测 | 少见，接受 |

---

## 输入：命中链 + 冒泡

输入的**所有**状态（hover / press / focus / capture）都活在 `Ui` 上，控件只读回。
结果是「亮起来的区域」和「响应的区域」在构造上是同一份数据，不可能漂移。

### 命中测试：绘制顺序的逆序，尊重 clip，用 base bounds

`collectHitChain` 深度优先、**先孩子后父、后兄弟先于前兄弟**（正好是 paint 顺序
的逆序），命中链最深节点在最前。三条约束：

- **用 base bounds**，不用 effective bounds（override 不能移动可交互区域）；
- **clip 容器内缩区域**：区域外连容器本身都点不到；
- **clip 容器自己的可点区域 = 它的 clip 矩形**（Stack 没有 render 对象，
  所以用 clip 当它的 bounds）—— 这也是 `clip()` 的语义。

### 一张表：每种事件的传播规则

| 事件 | 规则 | 代码 |
|------|------|------|
| MouseMove | 若某节点持有 capture，先喂给它（哪怕指针已出界）；再更新 hover，hover 变了才标脏 | `dispatchEvent` |
| MouseDown | 沿链找第一个处理者；焦点给**最深的 focusable**（与谁处理点击无关）；注册 `onDrag` 的节点夺 capture 并立刻收到一次 `onDrag` | `mouseDown` |
| MouseUp | 若 release 仍在 pressed 节点所在**子树**内才算点击（拖出去松手不算）；释放 capture 并调 `onDragEnd` | `mouseUp` |
| MouseWheel | 沿链冒泡，第一个**返回 true** 的消费；都不处理就作废 | `mouseWheel` |
| KeyDown | Tab → `focusNext`；其它键从**聚焦节点**沿父链冒泡 | `keyDown` |

### 四个刻意的取舍

1. **hover 在 build 之前重算。** 指针没动、但树可能动过，所以 `update()` 在
   调用 build 之前用上次的指针位置重跑命中测试。这样 build **一次**就拿到正确 hover，
   不需要第二遍或「下一帧才正确」。
2. **行为表每次 rebuild 重建。** `behaviors_` 是 build 声明的，节点不再被发射
   就不再响应 —— 不需要手动注销。
3. **滚轮到端点返回 false。** 滚动容器在已经到头时**不吞**事件，让外层容器接管，
   于是嵌套滚动成立。
4. **capture 在鼠标离开客户区后仍然存活。** 平台仍会把 release 送进来，
   拖出窗口再松手能正确结束拖动。

### 一个必须记住的控件约定

**同一个 key 只能注册一次行为。** `b.behavior(key, …)` 是「写」不是「追加」，
第二次会覆盖第一次。滑块就踩过这个坑：拖动和方向键本可以是两次注册，现在是
**一张 `NodeBehavior` 同时带 `onDrag` 和 `onKey`**（`controls.cpp` 有注释）。

---

## state：显式索引槽

M0 的状态模型是**显式编号的槽**，`Ui` 持有 32 个：

```cpp
int64_t state(uint32_t slot, int64_t initial) const;
void    setState(uint32_t slot, int64_t value);   // 唯一的 rebuild 触发点
```

`setState` 写**相同值**是 no-op（不标脏）。位置式的 `@State` 绑定要等 DSL 层
（M1 的 slice F），M0 的标准不依赖它。

---

## 谁拥有树，谁拥有绑定

```cpp
ElementPtr root_;          // Ui 持有 shared_ptr 根
NodeRef    find(Key);      // 弱引用，可能失效
OverrideToken             // 弱引用 + generation，可能失效
```

- **Element 归 `Ui`**（shared_ptr 树）；
- **`NodeRef` / `OverrideToken` 是弱引用**：节点没了就失效，绝不悬垂；
- C ABI 那侧把同一语义换成 **代际索引句柄**（不引入 shared ownership），
  见 [C ABI 与 Python 绑定](python.md)。

`Ui::shutdown()` 是幂等的（析构也会调），`host::run` **故意不**在循环结束时
shutdown —— 调用方（m0 的断言）要在 run 之后继续看树。

---

## 验证：这一层怎么被证明

框架侧的行为全部能**无窗口、无 GPU** 地断言，这是刻意的基建（[踩坑与注意事项](gotchas.md)）：

| 探针 | 覆盖 |
|------|------|
| `m0 --selftest` | 三棵树的 retained、两条路径共存、代际句柄 |
| `controls_gallery --input-test` | 交互：点击/拖拽/焦点/滚轮/虚拟化/裁剪命中 |
| `controls_gallery --layout-test` | 布局：padding/gap/flex/align/绝对子树平移/命中用布局位置 |
| `controls_gallery --paint-test` | 绘制顺序交错 + scissor 有无意外 |
| `bindings/python/controls_test.py` | 同一套控件经 C ABI 的头less 验证 |

## 接着读

- [渲染管线](rendering.md) —— 树翻译成图元之后发生什么
- [两条变更通道](two-paths.md) —— base / override 的用户视角
- [布局](layout.md) —— row / column 的用法与限制
- [组件设计](components.md) —— 输入系统为什么必须先进 core
