# “折光因果透镜可拆卸编程范式”与 `tokmon-n` 客观评估

> 评估日期：2026-08-25  
> 评估对象：本工作区中的 RCLD/Tokmon 文档、`tokmon-n` 最新源码与已有构建、Cordis 论文与源码、`deepseek-harness` 源码  
> 结论性质：架构与实现审查，不是同行评审、形式化验证报告、安全认证或跨平台产品验收

## 1. 执行摘要

### 1.1 一句话结论

**“折光因果透镜可拆卸编程范式”目前更适合被定义为一种有价值的代理系统工程纪律与架构综合，而不是已经完成严格证明、并在理论上取代 Cordis 的新编程范式；`tokmon-n` 则是一个真实、结构清晰、测试覆盖相当可观的 Windows 优先工程原型，但离论文宣称的“零残留、零幻觉、工业级完备实现”仍有明显证据与实现距离。**

### 1.2 最重要的判断

1. **思想值得保留。** `Fact → Lens → Surface/Act → Beam → Fact` 把事实、模型可见投影和物理作用分离，配合 append-only Photon、不可变 LightPath epoch、结构化 Act、进程隔离与 afterglow，确实适合长生命周期、可重构的智能体运行时。
2. **当前正式设计比论文更可靠。** [DESIGN.md](DESIGN.md) 已主动收缩论文中的绝对化表述，明确“无状态”的工程含义、现实不会被撤销、拆卸是“未来零新增贡献”、C++20 基线以及不承诺未经验证的 benchmark。这是正确方向。
3. **RCLD 论文的形式化部分尚不成立为完整元理论。** 其 Lens 类型、GetPut/PutGet/PutPut 公理和“追加 Photon”的 `refract` 定义互相冲突；操作语义不封闭；零残留、无死锁、合流性证明遗漏了关键假设。
4. **对 Cordis 的批评不够公平。** Cordis 论文没有声称所有物理世界结果都可逆；它明确区分系统边界内的可恢复 acquisition 与边界外的 emission，并讨论 withholding、compensation、恶意插件隔离、依赖环和版本漂移。RCLD 解决的是另一组问题，不是对 Cordis 的简单代际替代。
5. **`tokmon-n` 不是 PPT 工程。** Photon 数据库有 UPDATE/DELETE 拒绝触发器和 hash chain；LightPath 使用完整快照校验与原子发布；C ABI 热替换、多语言 worker、真实文件/Git/PTY/HTTP/MCP/LSP、Credential Manager 等都有代码与测试。
6. **但核心资源托管尚未完整落地。** 当前 C++ `OpticalHost` 只暴露 `emit` 和 `log`，设计中的 `MountGuard`/Hosted I/O/Task/Secret 统一资源托盘未形成实际强制边界；同进程 Lens 仍能直接调用 OS API，并需要自行停止所拥有的资源。
7. **现有证据支持“可信原型”，不支持论文中的极限指标。** 本次复跑已有 Windows 测试二进制得到 `85 cases / 2965 checks / 0 failures`，但没有可复现的零幻觉对照实验、跨平台完整验收、WASM live test、Docker live test或严谨性能统计。

### 1.3 启发式评分

以下分数是基于本文证据维度的审查量表，不是行业标准，也不应被理解为精确测量。

| 对象 | 维度 | 评分（10 分） | 判断 |
|---|---:|---:|---|
| RCLD 思想 | 问题选择与工程直觉 | 8 | 抓住了智能体上下文污染、动态能力卸载、审计与副作用边界等真实问题 |
| RCLD 思想 | 概念原创性 | 5 | 有新的组合和命名，但主体可追溯到 event sourcing、CQRS/投影、functional core/imperative shell、capability security、immutable snapshot 与 process isolation |
| RCLD 论文 | 形式化严谨性 | 3 | 定义、公理、类型和证明之间存在实质缺口 |
| RCLD 论文 | 实验可信度 | 2 | 缺少实验协议、原始数据、重复次数、置信区间和可执行对照 |
| `DESIGN.md` | 架构一致性 | 8 | 三平面、epoch、Act admission、afterglow 和多语言边界形成了较完整的工程闭环 |
| `tokmon-n` | 当前实现覆盖 | 6.5 | 核心链路真实，但资源托管、暗路验收、跨平台与可选后端仍有落差 |
| `tokmon-n` | 本地测试证据 | 7 | Windows 已有构建测试规模和现实边界覆盖较好，但不是当前 dirty source 的全量 clean rebuild，也不是所有平台验收 |
| `tokmon-n` | 生产成熟度 | 4.5 | 可作为高级原型/研究运行时继续投入，尚不宜按论文的工业级承诺对外背书 |

## 2. 范围、方法与证据边界

### 2.1 主要材料

本次评估优先使用以下材料：

- [折光因果透镜可拆卸编程范式论文](detachable-refractive-causal-lenses-paper.zh.md)
- [Tokmon 透镜架构白皮书](tokmon-lens-architecture-explained.zh.md)
- [Everything Is a Lens](everything-is-a-lens-paper.zh.md)
- [advise.md](advise.md)
- [20.md](20.md)
- [当前规范 DESIGN.md](DESIGN.md)
- [当前实现报告 IMPLEMENTATION-REPORT.md](IMPLEMENTATION-REPORT.md)
- `tokmon-n` 的 Nyxia、SDK、Lens、worker、runtime 与测试源码
- 工作区根目录的 [A Programming Paradigm for Spatiotemporal Composability](<../../A Programming Paradigm for Spatiotemporal Composability.pdf>)
- 工作区中的 `cordis` 与 `deepseek-harness` 仓库

### 2.2 文档权威层级

本评估采用 `DESIGN.md:15-22` 自己规定的优先级：

1. `DESIGN.md` 是当前规范；
2. 解释性文档次之；
3. 论文和 `advise.md` 只在与当前纯透镜语义一致时有效；
4. 旧版 Tokmon 文档只提供需求与历史背景。

这一区分非常重要。若把所有文档都当作同时有效的规范，会得到互相矛盾的系统：一边说 Lens 绝不持有任何状态或缓存，一边又实现缓存、socket、线程和连接；一边说所有 20 个构件都是动态 Lens，一边又把 Nyxia 定义为不可拆卸静态内核；一边使用 C++23，一边规定全工程只用 C++20。

### 2.3 本地版本快照

| 仓库 | 本地 HEAD | 时间 | 说明 |
|---|---|---|---|
| `tokmon-n` | `e243077b92c52a0253a1ca8d2dd5f60cffbfe907` | 2026-08-25 20:41 +08:00 | `update ui` |
| `cordis` | `8cc9e33fab69e2d0476d126baaf2acb24e6a6ab4` | 2026-08-13 21:48 +08:00 | `chore: update readme (#45)`；core 包版本 `4.0.0-rc.8` |
| `deepseek-harness` | `b150a551b8d465e31e418e1b2eaf5e79bbb7d28e` | 2026-08-21 20:03 +08:00 | `dsh-0.1.1-rc.2` 合并提交 |

`tokmon-n` 检查时存在 4 个 Desktop UI 源文件修改，以及未跟踪的 RCLD 论文文件。本评估没有修改或清理这些用户变更。因而“最新实现”指本地工作区可见源码；测试结论则明确限定为现有构建目录中的二进制，而非该 dirty worktree 的 clean rebuild。

### 2.4 已执行验证

在 `tokmon-n` 现有 Windows 构建目录上执行：

```text
ctest --test-dir build/windows-msvc-ui-debug --output-on-failure -C Debug
```

结果为 3/3 CTest target 通过，包括：

- `tokmon-tests`
- `sdk-node-contract`
- `sdk-cpython-contract`

直接复跑核心测试二进制的汇总为：

```text
[chtest] cases=85 subcases=0 checks=2965 failures=0
timeouts=0 retries=0
```

需同时说明：

- 这证明现有构建产物在本机 Windows 环境可运行；
- 它没有证明所有当前 UI 未提交修改都已重新编译；
- `IMPLEMENTATION-REPORT.md` 中的 `84/84` 已落后于当前测试二进制的 85 个用例；
- `cordis` 与 `deepseek-harness` 本地没有安装其完整 Node 依赖，本次没有宣称运行了它们的测试套件；
- 没有进行真实 LLM 多轮“幻觉率”对照实验，也没有重新跑论文声称的内存和卸载延迟 benchmark。

## 3. 这套范式究竟是什么

### 3.1 去掉光学比喻后的工程模型

把术语翻译成常见软件架构语言后，RCLD/Tokmon 的核心是：

| Tokmon 术语 | 工程含义 | 相近既有概念 |
|---|---|---|
| Photon / Fact | 带 schema、provenance、因果关系和 epoch 的不可变事件 | event sourcing、audit log |
| Causal Ray | 一次任务或会话的因果事件子流 | trace/session/aggregate stream |
| Lens `view` | 从事件窗口派生模型提示、工具 schema、UI 或中间投影 | CQRS projection、fold、selector |
| Surface | 当前 epoch 下临时构造的可见上下文 | materialized/ephemeral view |
| Act | 结构化、可审计、待策略准入的副作用意图 | command/effect request |
| Beam `refract` | 在授权和生命周期约束内执行 Act，并追加结果事件 | imperative shell/effect handler |
| LightPath | 有序组件集合的不可变运行快照 | immutable routing/config snapshot |
| epoch | 配置代际和模型可见能力边界 | generation/versioned configuration |
| afterglow | 旧代停止接新工作、等待在途工作、最终停止 | graceful drain/quiescing |
| dark lane | 发布前对候选组件进行隔离验收 | staging/canary/preflight |

它真正重要的不是术语，而是三个边界：

1. **Fact 平面**只追加已经发生的事实；
2. **Lens 平面**只从事实派生当前能力与上下文；
3. **Act 平面**把所有外部作用收敛到可审计、可拒绝、可隔离的执行路径。

### 3.2 “可拆卸”的三种含义必须分开

当前文档经常把以下三个命题混在一起：

1. **认知拆卸**：新 epoch 的模型 Surface 不再直接包含已卸载 Lens 注册的 prompt/tool/UI contribution；
2. **生命周期拆卸**：Lens 不再接收新 Beam，旧 Beam 被 drain/cancel，托管资源最终释放；
3. **现实回滚**：历史文件写入、消息发送、交易或网络结果被撤销。

Tokmon 的合理目标是前两项，不应该承诺第三项。`DESIGN.md` 已经明确“现实不会被撤销”和“历史不被删除”，这是比论文更准确的语义。

### 3.3 最可辩护的范式表述

相对客观、可证伪的版本应是：

> 对一个由 append-only 事实流派生模型 Surface 的系统，若某 Lens 的直接贡献带有可识别 provenance，所有模型请求都从当前不可变 LightPath epoch 重新构造，且所有新副作用只能通过当前 epoch 的 Act/Beam 边界发起，那么从 Lens 被移出新 epoch 开始，它不再产生新的直接认知贡献或新的 Act；旧 epoch 的在途工作按明确 afterglow 策略收敛，历史事实和已经发生的外部结果不被隐式删除或伪装为回滚。

这比“物理摘除后所有相关 token 绝对为空、从第一性原理彻底杜绝幻觉”弱，但更准确、更可实现，也更适合成为产品契约。

## 4. 与 Cordis 论文的客观对比

### 4.1 Cordis 论文真正解决的问题

《A Programming Paradigm for Spatiotemporal Composability》定义了两类组合性：

- **时间组合性**：组件移除时，恢复其对共享上下文造成的、被系统跟踪的作用；
- **空间组合性**：组件声明对 context/coeffect 的依赖，运行时根据供给变化自动激活、卸载或重载。

其核心 effect 类型可概括为：

\[
f : \Gamma \to \Gamma \times (\Gamma \to \Gamma)
\]

组件执行作用时同时交回一个左逆；运行时按逆序组合这些逆操作。reactive coeffect 则把依赖供给纳入运行上下文，使组件 activation 与依赖 epoch 绑定。

这不是普通的“每个插件作者自己写一个 uninstall 函数”。Cordis 的价值在于：通过 context API 收集作用和 disposer，以 LIFO 方式结构化清理，并把依赖变化与生命周期重载连起来。

### 4.2 核心差异矩阵

| 维度 | Cordis 范式 | RCLD/Tokmon | 客观判断 |
|---|---|---|---|
| 基本状态 | 可变化的共享 context `Γ` | append-only Photon 加派生 Surface | 关注点不同：Cordis 管共享运行状态；Tokmon优先保存事实并重算可见面 |
| 时间组合 | 被跟踪作用附带 inverse/disposer，逆序恢复 | 新 epoch 停止未来贡献；旧代 afterglow；历史保留 | Cordis 对“被跟踪的宿主状态恢复”更强；Tokmon 对“历史不可抹除、未来能力摘除”更诚实 |
| 空间组合 | reactive coeffect / IoC dependency graph | 有序 LightPath、schema/permission/conflict 校验 | Cordis 更适合开放式服务依赖；Tokmon 更强调可预测的代理处理管线 |
| 拓扑 | 声明式依赖图；循环会保持不活跃并可检测 | 主要是线性 fold，内部 Lens 仍可有 DAG/工作流 | 线性光路降低全局路由复杂度，但没有消灭能力内部的图结构 |
| 外部副作用 | 系统边界外 emission 不自动可恢复；建议 withholding/compensation | Act admission、sandbox、append result；不删除现实 | 两者并不矛盾，Tokmon 是把边界做成更显式的代理运行时协议 |
| 卸载后的模型上下文 | 不是论文的主要对象 | 是一等目标 | Tokmon 在 LLM cognitive surface 上更聚焦 |
| 持久审计 | Cordis core 本身不要求持久 event store | SQLite Photon、hash chain、审计事件 | Tokmon 在当前原型中更强 |
| 资源清理 | 所有经 `effect()` 注册的 disposer 逆序调用 | 设计为 MountGuard 托管；当前实现部分依靠 Lens 自行 stop | 现实现状下 Cordis 的 tracked-effect 机制更直接、闭环更完整 |
| 安全边界 | context 拦截有 capability 倾向；恶意代码需进程/容器沙箱 | T1 同进程 Lens + worker/Job Object/策略 Lens | 两者都不能仅靠函数接口约束恶意本机代码；Tokmon worker 路径隔离更具体 |
| 形式化成熟度 | 有明确演算、preservation/progress/confluence 及显式前提 | 有公理和定理草图，但定义与证明未闭合 | Cordis 论文目前明显更严谨 |
| 产品场景 | 通用插件生态、服务注入、HMR | 长生命周期 AI agent、模型 Surface、工具与沙箱 | Tokmon 的垂直问题选择合理，不必宣称全面取代 |

### 4.3 RCLD 对 Cordis 的批评哪里合理

以下批评成立或部分成立：

1. **inverse/disposer 只覆盖被正确登记的作用。** 插件绕过 context 直接操作全局变量、文件、线程或外部系统时，Cordis 无法凭空恢复。
2. **物理世界通常只能补偿，不能严格求逆。** 已发送消息、已完成支付和远端不可撤销动作不能被数学左逆真正抹除。
3. **LLM 上下文污染不是一般组件卸载理论自动解决的。** 如果模型请求拼装还读取旧缓存或不可追踪的全局注册表，组件 disposer 执行完也可能继续显示旧 tool schema。
4. **依赖图在大型生态中有作者负担。** Cordis 论文自己承认，把双向交互拆成单向 integration component，组件数在最坏情况下可二次增长。

### 4.4 哪些批评不够公平

RCLD 论文 `detachable-refractive-causal-lenses-paper.zh.md:87-90, 642-646` 倾向把 Cordis 描绘成“假定所有物理副作用都可逆”和“必须用 DAG 处理所有执行”。这与原论文不完全一致：

- Cordis 在 PDF 第 67-68 页专门定义系统边界。边界外 emission 在 `Γ` 上表现为恒等，既不被跟踪也不被恢复；对其使用 withholding 或 compensation。
- Cordis 对循环的结论不是“运行时死锁”，而是依赖满足条件永远不成立，组件保持 inactive；这种情况可以静态报告。
- 论文第 69 页明确说 capability-like context 不能对抗恶意组件，后者需要进程或容器等外部隔离。
- 论文第 71-72 页主动讨论 mutual dependency 的粒度成本，以及 nominal key 的 interface drift、key collision 与版本问题。

因此更准确的关系是：**Cordis 提供“可恢复宿主作用 + 反应式依赖”的通用组合底座，RCLD 试图为 agent context 和不可逆现实建立“历史保留 + 当前投影摘除 + 统一 Act 边界”。二者可组合，不能简单排成新旧两代。**

## 5. RCLD 形式化部分审查

### 5.1 值得肯定的部分

1. **把“模型认知表面”作为显式语义对象。** 传统插件系统通常只讨论服务注册、事件监听和资源回收；RCLD 进一步要求工具 schema、系统提示、检索片段、UI 投影都能按 epoch 重建。
2. **把 provenance 纳入事件。** Photon 的 sequence、origin、type/schema、payload、causes/Act 关联为后续审计、回放和归因提供了基础。
3. **拒绝伪造现实回滚。** 只追加事实流比对不可逆结果强行造 inverse 更符合代理系统实际。
4. **把拆卸变成一次快照切换。** 若所有请求确实从当前 epoch 获取完整 LightPath snapshot，就能避免读者看到半更新注册表。

### 5.2 Lens 类型和公理存在实质冲突

RCLD 论文定义：

\[
view : S \to A, \qquad refract : S \times B \to S'
\]

随后给出：

\[
refract(s, view(s)) \equiv s
\]

这里至少有三个问题：

1. `view(s)` 的类型是 `A`，而 `refract` 接受 `B`；除非声明 `A = B` 或提供显式转换，否则 GetPut 不可类型检查。
2. 论文又把 `refract` 解释为把新 Photon 追加到流。如果 `view(s)` 被追加为新事实，结果一般不可能严格等于 `s`。
3. PutGet 中引入 `Π_B(b)`，但没有给出足以支持后续证明的完整定义与约束。

当前接口更接近“只读 projection + 命令解释器/effect handler”，不是经典 total lens 的 `get/put`。最干净的修订方式不是硬套 GetPut/PutGet/PutPut，而是像 `DESIGN.md:203-211` 那样直接采用工程定律：append-only、pure view、Act-before-reality、no side channel、linear fold、patterned refraction 和 darkness。

### 5.3 “Profunctor Optic”主张证明不足

论文给出一个 optic 风格的 coend 表达式，但具体的 `view/refract` 对并不会仅因为写出该表达式就自动成为 lawful Profunctor Optic。需要进一步给出：

- 使用的 category、profunctor class 和 residual；
- concrete representation 到 optic encoding 的同构；
- `A/B/S/T` 的完整类型关系；
- 对应 lawfulness 条件；
- Photon append effect 如何与纯 optic 分层。

否则“Profunctor”主要承担修辞作用，而非实际证明作用。

### 5.4 自由幺半群、自由单子和因果偏序混用

`detachable...:151` 把 `Φ` 同时称为“因果流自由单子”和“因果偏序约束下的自由幺半群”。自由 monoid 和 free monad 不是同一概念；加入因果偏序或 quotient 后，也必须说明生成元、等价关系、组合是否良定义以及 identity/associativity 如何保持。

工程实现其实不需要如此重的代数包装。一个由 sequence、parent/caused-by 和 schema 组成的 append-only event log，加上对 snapshot 的确定性 fold，已经足够表达主要契约。

### 5.5 复合定理只给出不完整证明

论文 `:213-228` 只展开了一个 GetPut 草图，然后称 PutGet、PutPut “直接展开即证”。但在前述类型不闭合、`refract` 又会追加事件的情况下，这两条不能略过。若不同 Lens 都能改变流，组合还需要讨论：

- contribution 的顺序是否可交换；
- 一个 Lens 是否读取另一个 Lens origin 的历史 Photon；
- schema 冲突和 Act target 冲突；
- nondeterministic I/O 的处理；
- epoch 跨越和旧 Beam 的线性化点。

### 5.6 RCLD 动态演算尚未形成封闭操作语义

论文给出的 8 条规则有助于传达意图，但还不足以支撑元理论：

- Lens 序列在部分规则中被当作集合使用，丢失线性顺序；
- fork 后状态元组的形状发生变化，没有统一配置语法；
- `Branch`、`Merge`、`ProofVerified`、`Compress` 等判断没有定义；
- compaction 会替换 `Φ`，与“历史严格 append-only”需要额外的 archive/checkpoint 语义协调；
- failure、timeout、cancel、approval、concurrent Beam、epoch publication 没有进入规约；
- 没有清晰给出终止状态、evaluation context 和 transition relation 的全域。

### 5.7 “零残留认知卸载定理”证明不成立

论文的证明核心是：Lens 是纯函数；移除 Lens 后重算 Surface；所以与该 Lens 有关的 token 必为空。这一步不成立。

反例很简单：Lens `P` 曾经追加一个带 `origin=P` 的历史 Photon；另一个仍在光路中的 Lens `Q` 会读取所有历史错误记录并把它们投影到 prompt。即使 `P` 和 `Q` 都是纯函数，移除 `P` 后，`Q(Φ)` 仍可输出与 `P` 有关的内容。

纯函数性只能保证相同输入得到相同输出，不能推出对某个历史 origin 的非干涉。要得到可证明结论，至少需要：

- 定义“直接 contribution”与“历史事实引用”的区别；
- 所有 Surface contribution 带 `lens_id/generation/epoch` provenance；
- 新 Surface 只接受当前 LightPath 的直接 contribution；
- 对跨 Lens 历史投影给出显式 allow/deny policy；
- cache 为空或严格按 epoch/version key 失效；
- 所有模型请求只从本次 Surface 构造，禁止旁路注册表。

因此 `DESIGN.md:1554-1566` 的“零新增贡献”语义是正确修订：**卸载不删除历史，不撤销现实，只保证新 epoch 不再接受该代 Lens 的新直接贡献。**

### 5.8 非干涉、无死锁和合流性结论缺少前提

#### 非干涉

“sandbox 分支被丢弃，因此主流不变”只对内存中的 Photon merge 成立。若分支已经写文件、发网络请求或改变外部服务，丢弃分支对象不会自动消除这些结果。必须依赖真正的 process/container/file-system isolation、withholding 或 compensation。

#### 无死锁/进展

线性 Lens 顺序不能自动保证进展：一个 `view`、LLM 请求、网络调用、subprocess 或 Lens stop 都可能无限等待。需要 finite timeout、cancel propagation、resource bound、scheduler fairness 和故障转移假设。

#### 合流性

自然停机只有在 transition 确定、独立动作可交换、没有时间/随机数/网络/模型非确定性，并且状态空间或预算有界时才可能有唯一 normal form。真实 LLM 和外部系统通常不满足这些条件。

Cordis 论文中的 preservation/progress/confluence 之所以更可信，是因为它明确列出 independence、acyclic precedence、finite names/bounded iterations、total provision 等条件。RCLD 若要主张同等级元理论，也必须把假设写进定理。

### 5.9 实验性主张目前不可接受

论文报告：

- 100.00% 零认知残留；
- 0.00% 工具调用幻觉；
- 0.18 ms 热拆卸；
- 内存长期 `< 50 MB`；
- 相对 Cordis/VS Code 的对照数据。

但没有提供：

- benchmark 源码和固定 commit；
- 硬件、编译器、优化级别、数据规模和 warm-up；
- 每组样本数、随机种子、模型版本、temperature；
- P50/P95/P99、方差或置信区间；
- “残留”“幻觉”“卸载完成”的操作性定义；
- 对照系统的等价工作负载和配置；
- 原始结果文件。

更严重的是，论文第 6 章宣称“完整无 Mock”，示例却明确写有“模拟大模型”并硬编码响应；所谓 `clean_surface` 的零残留验证没有真正重新计算或断言 Surface，只打印预期结果。因此这些数字只能视为未经验证的作者主张，不能作为架构优越性的证据。

## 6. 文档体系的一致性评估

| 文档 | 实际性质 | 优点 | 主要问题 | 建议状态 |
|---|---|---|---|---|
| `advise.md` | 头脑风暴/架构讨论记录 | Fact-Lens-Act 的直觉来源清楚 | 大量“终极、碾压、绝对”等宣传语；示例与无 Mock 主张矛盾 | 标记为 historical/ideation，禁止作规范引用 |
| `20.md` | 旧模块需求与能力清单 | 保留了产品需求和 20 个命名来源 | 仍含 Context/Fiber、event bus 等旧语义，与纯透镜现规范冲突 | 标记为 legacy requirements |
| `everything-is-a-lens-paper.zh.md` | 早期架构论文 | 光学统一叙事完整 | 重复经典 lens law 和证明问题；绝对化结论过多 | 改为 vision paper |
| `tokmon-lens-architecture-explained.zh.md` | 教程/解释文 | 对用户理解三平面有帮助 | 同时声称“绝无缓存”和增量缓存；“永不死锁”“<2ns”等未证实；代码仍模拟模型 | 保留教程，删除强 benchmark 与 theorem 口吻 |
| `detachable...paper.zh.md` | 研究论文外观的白皮书 | 集中表达 RCLD 理念 | 形式化与实证均未达到论文声称强度 | 改名 technical whitepaper / research agenda |
| `DESIGN.md` | 当前规范 | 最自洽，明确非目标、现实边界、C++20 与工程化无状态 | 部分设计尚未落地，篇幅很长 | 继续作为唯一 normative spec，增加 requirement IDs |
| `IMPLEMENTATION-REPORT.md` | 实现快照 | 能诚实列出 Wasmtime/Docker/平台限制 | 测试数已过期；部分“完成”措辞仍应绑定 commit/build | 自动生成版本、commit 和 test manifest |

建议在每篇文档页首统一加 front matter：

```yaml
status: normative | explanatory | vision | historical
applies-to-commit: <git sha>
superseded-by: <path or null>
claims-evidence: <benchmark/test artifact or null>
```

这会显著降低新开发者把旧概念误认为现行契约的风险。

## 7. `tokmon-n` 实际实现评估

### 7.1 真实实现与规范映射

| 规范主张 | 当前实现证据 | 状态 | 备注 |
|---|---|---|---|
| Photon 物理只追加 | `nyxia/storage/photon_store.cpp:115-139` 建表并用 trigger 拒绝 UPDATE/DELETE；`:161-204` 分配 sequence、previous hash、内容 hash 后 INSERT | 已实现 | `tests/unit/core_tests.cpp:137-165` 验证 hash chain 和 SQL 物理拒绝 |
| LightPath 不可变 epoch 原子发布 | `nyxia/light_path/light_path.cpp:29-117` 使用 snapshot、完整候选校验与 publish | 已实现 | 并发读者持有 shared immutable snapshot，设计方向正确 |
| Lens 只接收 PhotonWindow/Surface/Beam | `sdk/cpp/include/tokmon/lens.hpp:57-104` | 基本实现 | Host 仍太窄，见资源托管缺口 |
| Act 先审计、准入、再执行 | `nyxia/engine/ray_tracing_engine.cpp:70-171` | 已实现 | proposed/admitted/started/completed/failed 形成可审计链路 |
| natural darkness | `ray_tracing_engine.cpp:174-215` | 部分实现 | 当前主要依据“无 proposal”熄灭，并有 beat budget；未完全覆盖规范中的 pending Beam/approval 等判定 |
| dark lane | `nyxia/runtime/runtime.cpp:660-706` | 部分实现 | 只对空 PhotonWindow 调一次 `view` 并验证候选 LightPath；尚非完整历史回放、合成 Act、停止界限与安全验收 |
| epoch 换代与 afterglow | `runtime.cpp:604-730` | 已实现基础路径 | 发布后旧代等待活动 Beam，超时后 `request_stop`，并追加 afterglow Photon |
| C ABI Lens 热替换 | loader/runtime 与 `core_tests.cpp:526-588` | 已实现并测试 | artifact hash 和 generation 改变后发布新 epoch |
| Node/Python worker | `nyxia/worker/worker_lens_proxy.cpp` 与 SDK contract | 已实现并测试 | 测试覆盖 worker RPC 和 host Beam emit |
| Windows 进程树终止 | `worker_lens_proxy.cpp:293-311, 456-462` | 已实现 | Job Object `KILL_ON_JOB_CLOSE`，优于只靠合作式插件回调 |
| 19 个正式 Lens + Calculator 示例 | `lenses/common/builtin_registry.cpp`、各 Lens 动态库与 contract tests | 已实现 | 数量口径与论文“20 个全量 Lens”不一致，需澄清 Nyxia/Calculator 的计数 |
| 所有能力可从 desired LightPath 移除 | `runtime.cpp:683-689` | 未完全满足 | 若 Calculator 不在候选中，runtime 会强制加回；这让参考 Lens 成为隐式常驻项 |
| MountGuard 统一托管资源 | `DESIGN.md:511-599` | 尚未完整实现 | 当前 SDK 中找不到对应完整宿主资源托盘实现 |

### 7.2 值得肯定的工程部分

#### 7.2.1 PhotonStore 的不变性是“物理约束”，不是注释

`photon_store.cpp` 不仅在 API 上没有 update/delete，还在 SQLite 层创建：

- `photons_no_update` trigger；
- `photons_no_delete` trigger；
- 全局 sequence；
- `previous_hash`；
- 内容 SHA-256 hash；
- 启动后 chain verification。

测试还绕过高级 API 直接执行 SQL update/delete，确认数据库拒绝。这是当前实现最扎实的差异化之一。它不能防止拥有数据库文件权限的恶意进程重建整个文件，但对应用内误改、普通插件 API 和审计完整性很有价值。

#### 7.2.2 LightPath 的发布模型清楚

当前实现先构造完整候选，执行 manifest/schema/dependency/conflict/permission 等检查，再一次性 publish immutable snapshot。Engine 每次处理持有一个 snapshot，因此不会在一次 fold 中读到半新半旧的 Lens 列表。

这比对全局 registry 做一连串 add/remove 更容易推理，也更适合为“模型请求使用唯一 epoch”建立线性化点。

#### 7.2.3 Act 生命周期具备可审计骨架

Engine 的 Act 流程不是让 Lens 直接执行任意工具调用：

1. 追加 `act.proposed`；
2. admission/policy；
3. 追加 `act.admitted`；
4. 获取 Beam ticket；
5. 追加 `act.started`；
6. target Lens `refract`；
7. 追加 `act.completed`、`act.denied`、`act.cancelled` 或 `act.failed`。

只要所有真实副作用都被强制通过这条路径，这个设计能同时支持审计、重放解释、approval binding、超时和卸载收敛。

#### 7.2.4 多语言 worker 与 Windows 生命周期控制不是占位符

现有测试实际启动 Node.js 和 CPython Lens，通过 WorkerLensProxy 解析协议并经 host Beam 追加 Photon。Windows worker 被放入 Job Object，宿主持有整棵进程树的最终终止权；stop 先发送 `worker.shutdown`，再走强制终止兜底。

这部分比“插件提供一个 disposer，然后相信它会正确清理”更适合不完全可信或可能挂死的扩展。

#### 7.2.5 测试覆盖了不少现实边界

85 个聚合用例不仅是纯单元函数。可见覆盖包括：

- append-only SQLite 和 hash chain；
- LightPath 并发原子快照；
- Calculator 完整 Fact-Lens-Act 循环；
- C ABI 动态库换代；
- Node/CPython worker；
- MCP、LSP、HTTP collector/Prometheus；
- PTY、进程取消、Windows Job Object；
- Windows Credential Manager 与 secret redaction；
- 真实 Git repository/worktree；
- 文件生命周期、RAG revision、memory provenance；
- 19 个正式 Lens 与 Calculator 的独立 contract。

这足以证明 `tokmon-n` 是可运行工程，而不是仅有接口和假实现。

### 7.3 主要实现缺口

#### P0：资源托管边界没有形成强制能力模型

`DESIGN.md:542-599` 把 `MountGuard` 描述为 Lens 的“镜座电源与资源托盘”，所有 timer、thread、socket、file handle、subscription、secret binding 都应由宿主持有和撤销。

但当前 `sdk/cpp/include/tokmon/lens.hpp:57-63` 的 `OpticalHost` 只提供：

- `emit(PhotonDraft)`；
- `log(...)`。

这意味着：

- 同进程 native Lens 可以绕过 host 直接调用 Win32/POSIX、文件系统或网络；
- host 无法枚举它创建的全部资源；
- 不能仅凭 `request_stop()` 证明资源都已释放；
- “插件作者不写清理逻辑”目前并不成立。

例如实际的 stateful Lens 仍会拥有 mutex、string、HTTP endpoint 等对象，并在 destructor/`request_stop()` 中手动停止。这里不是说 Lens 不能有工程状态，而是规范声称的统一 custody 尚未实现。

建议优先级最高的修复是：

1. 扩展 host 为 typed HostedTasks/Timers/IO/Secrets/Photon/Act API；
2. 每个 mount 拥有可枚举 guard；
3. 关闭代际时先拒绝新 ticket，再 cancel/close guard 下资源；
4. 对 T1 同进程 Lens 明确标注“可信代码”，不要把接口约束描述成安全沙箱；
5. 对 T2/T3 扩展默认使用 worker/process boundary。

#### P0：论文级性能和幻觉主张没有工程证据

当前测试证明功能正确性的一部分，不证明：

- 卸载 P99 为 0.18 ms；
- 常驻内存长期低于 50 MB；
- 工具幻觉为 0%；
- 相对 Cordis/VS Code 的性能优势。

这些数字应在所有对外文档中暂时删除或标成 hypothesis，直到 benchmark 可复跑。

#### P0：Calculator 被运行时强制加入候选路径

`runtime.cpp:683-689` 会在候选不含 Calculator 时自动插入内置 Calculator。这违背“desired LightPath 是实际能力选择结果”以及“一般 Lens 可拆卸”的直觉，也会污染零新增贡献测试。

建议让 Calculator 只存在于示例配置/测试 fixture，不在 runtime reconciliation 中硬编码。

#### P1：dark lane 仍是 smoke test，不是完整发布前验证

当前 dark lane 主要做空窗口 `view`。空输入不会发现：

- 历史 schema 解析崩溃；
- 特定 Surface contribution 冲突；
- Act 在真实权限集下绕过 admission；
- `refract` timeout/cancel 不合作；
- 资源泄露和 stop bound；
- 非确定性 view；
- worker 大输出、IPC framing、恶意 payload。

建议至少加入：历史采样回放、固定 synthetic Act、两次 view equality、stop deadline、resource ledger delta、worker crash/timeout 与 schema fuzz。

#### P1：natural darkness 判定比规范简单

现 Engine 在没有 Act proposal 时熄灭，并受 max beats 约束。完整定义还应考虑：

- 是否存在待完成 Beam；
- approval 是否 pending；
- streaming model/tool 是否仍产生增量；
- child ray/join 是否未完成；
- repeated equivalent Act 是否形成振荡；
- external wake-up 是否属于本 ray 的可达事件。

现有实现可以称“有 budget 的无提案终止”，不宜直接等同于已经证明的全局 quiescence。

#### P1：跨平台和可选后端仍未完成现场验收

`IMPLEMENTATION-REPORT.md:204-209` 已诚实记录：

- 本机无 Wasmtime CLI，未执行真实 `.wasm`；
- Docker daemon 未运行，未完成 live container 测试；
- 完整现实系统测试集中在 Windows；
- 非 Windows credential backend 未实现；
- 非 Windows 敏感 Blob envelope 未配置时 fail closed。

Fail closed 是正确选择，但仍不能把“路径已实现”写成“所有后端已验收”。

#### P1：进程内 purity/no-side-channel 主要依靠纪律

`ILens::view` 的 C++ 签名限制了显式 host 能力，却不能阻止 Lens：

- 读取全局变量、当前时间或随机数；
- 使用静态 cache；
- 直接写文件或网络；
- 启动线程；
- 从环境变量读取 secret。

contract test 可以发现已知 Lens 的一些违规，不能对任意 native library 提供语言级证明。应把“纯 Lens”定义为可审计 contract，对不可信实现用 worker sandbox 强制，而不是把 C++ virtual interface 当成 purity proof。

#### P2：数量与身份口径不统一

当前实现报告说“十九个正式内置透镜与 Calculator 参考透镜”；论文说“20 个构件模块均是 Lens”，同时又把 Nyxia 视为底座。建议固定表述：

- Nyxia：静态微内核，不是动态业务 Lens；
- 19 个正式业务 Lens：可配置；
- Calculator：参考/测试 Lens，不属于正式 19；
- 若发布包恰好有 20 个 Lens 动态库，不要把它与“20 个正式构件”混称。

## 8. 与 Cordis 源码实现的比较

本地 Cordis core 的关键机制与论文基本对应：

- `packages/core/src/fiber.ts:275-339` 的 `effect()` 收集 disposer，并按逆序执行；
- `fiber.ts:385-413` 根据注入 provider 的 uid 计算依赖 epoch；
- `fiber.ts:415-457` 在 epoch 变化时 reload/unload，并清理登记的 effects；
- registry/reflect/events/logger 都通过 context/fiber effect 接入生命周期。

### Cordis 相对 `tokmon-n` 的优势

1. **理论到代码的映射更直接。** paper 的 effect/coeffect/fiber 在 core 源码中能找到紧凑对应。
2. **开放插件服务依赖更自然。** provider/consumer 通过 context 注入，无需把所有交互硬塞进单一线性数据面。
3. **登记作用的清理语义成熟。** 对 listener、timer、service registration 等宿主内可恢复效果，LIFO disposer 很实用。
4. **TypeScript/JavaScript 生态的 HMR 开发体验更成熟。** 对普通应用插件系统，Cordis 的抽象成本较低。

### Cordis 相对 `tokmon-n` 的局限

1. 没有经过 context/effect 注册的作用不会自动恢复；
2. core 不提供 Tokmon 式持久 append-only 审计链；
3. 默认同进程 TypeScript 运行时不是恶意插件隔离；
4. nominal key 在独立发布组件之间有 interface drift/key collision；
5. 循环依赖需要重构为更细 integration components，作者认知负担可能上升；
6. 它没有把 LLM prompt/tool surface 的“当前可见性”作为原生一等对象。

### 合理的组合方向

最有价值的方向不是让两者互相否定，而是：

- 用 Tokmon 的 Photon/Surface/Act/epoch 作为 agent data plane；
- 用 Cordis 式 reactive coeffect 管理宿主服务依赖；
- 用 effect/disposer 处理明确可恢复的 host-local resource；
- 用 MountGuard/worker 处理需要强制 custody 或不可信代码的 resource；
- 对外部 emission 统一走 Act admission、withholding/compensation 与审计。

## 9. 与 `deepseek-harness` 的比较

### 9.1 架构重叠比表面看起来更大

`deepseek-harness/docs/architecture.md` 已明确：

- Cordis 是底层插件框架；
- everything is a plugin；
- session 是 append-only event-sourced log；
- model-visible means logged；
- prompt、tool、agent loop、session、LLM adapter 都可替换；
- capability seam 由 Service Definition / Provider / Consumer 三个角色构成；
- tool/policy/sandbox/session/profile/bundle 都有独立扩展面。

这与 Tokmon 的对应关系是：

| Tokmon | deepseek-harness |
|---|---|
| Photon/Causal Ray | SessionEvent append-only log/session |
| Surface | 从 session log 渲染的 model request context |
| Lens contribution | plugin 注册的 prompt/tool/service/event contribution |
| Act | model tool call / tool pipeline request |
| Fallen/Styx/Cista | policy、permission、sandbox、secret capability packages |
| desired LightPath | profile/bundle/config composition |
| afterglow | Cordis scope/fiber disposer 与插件生命周期 |
| Aya child ray | subagent/session scope/delegation packages |

所以不能把 `deepseek-harness` 当成“传统不可拆插件框架”。它已经实现了事件日志派生上下文和 scoped reversible effects，只是没有采用 Tokmon 的光学词汇与严格线性 Lens 统一接口。

### 9.2 `deepseek-harness` 的优势

1. **产品能力面远大于当前 Tokmon。** LLM、shell、web、browser、computer、subagent、memory、skills、MCP、ACP、daemon、OAuth、policy、sandbox、UI 等形成了大量独立包。
2. **扩展角色清楚。** Service Definition/Provider/Consumer 能表达开放能力生态，而不要求每个插件既投影又执行。
3. **模型可见性规则与 Tokmon 同样重视可重建性。** “model-visible iff logged” 是很强的运行时不变式。
4. **真实自修改与配置工具更靠前。** tool-cordis 等扩展允许模型定义、运行、停止和移除插件；Tokmon 的“自进化操作系统”更多仍是设计路线。
5. **测试与工程治理规模更大。** 本地仓库仍标注 developer preview/RC，不能称稳定产品，但其 package/test/doc 深度显著高于当前 `tokmon-n`。

### 9.3 `tokmon-n` 的潜在优势

1. **数据与审计边界更统一。** PhotonStore 把 agent 事实、Act 生命周期、epoch 换代和 provenance 纳入同一持久流。
2. **物理 append-only 更强。** SQLite trigger + hash chain 比单纯内存 append-only 或普通 JSONL 更容易检查篡改和误写。
3. **原生宿主与 process worker 的统一目标更明确。** 若 MountGuard 最终落地，Tokmon 可以在 native、Node、Python、WASM 间提供一致的资源托管语义。
4. **线性 projection 对调试友好。** 当前 epoch 的 Lens 顺序、每个 contribution 和目标 Act 较容易做 inspector/replay。

### 9.4 `tokmon-n` 的劣势

1. **固定 19 类 Lens 容易把产品 taxonomy 当成编程模型。** 新能力若不属于预设光学角色，扩展方式不如通用 service/event/tool seam 自然。
2. **“所有东西都是同一种 Lens”可能造成错误统一。** projection、policy、storage、process sandbox、UI、transport 的生命周期和代数性质并不相同。
3. **生态和生产证据差距大。** 当前 Tokmon 更像 coherent prototype；DSH 已有更广泛的 package、provider、tool 与集成测试资产。
4. **资源托管的实现落后于文档。** 在这一点补齐前，Tokmon 还不能以“自动拆卸所有资源”胜过 Cordis scope/disposer。

## 10. 三者的相对定位

| 目标 | Cordis | deepseek-harness | RCLD / `tokmon-n` |
|---|---|---|---|
| 通用动态组件组合理论 | **强** | 继承 Cordis | 当前偏弱，形式化未闭合 |
| 插件服务依赖与 HMR | **强** | **强** | 中等，偏 snapshot reconciliation |
| Agent 产品能力完整度 | 弱/非目标 | **最强** | 中等，19 Lens 覆盖广但深度不一 |
| 模型上下文可追溯 | 非核心 | **强** | **强设计，已有真实存储基础** |
| 持久事实防误改 | 非核心 | 中等，append-only session | **强，SQLite trigger + hash chain** |
| 同进程作用清理 | **强，对已登记 effect** | **强，继承 Cordis** | 当前中等，规范强于实现 |
| 不可信扩展隔离 | 需外部机制 | 有多种 sandbox/provider | worker 路径较强，同进程路径仍靠信任 |
| 单一执行链可解释性 | 中等 | 中等，事件/服务较多 | **强，线性 LightPath 与统一 Act** |
| 当前形式化证据 | **最强** | 主要是工程 | 最弱，需要重写 |
| 当前生产成熟度 | core RC | developer preview/RC，但规模最大 | 高级原型/早期预览 |

这里的“强/弱”均限定在对应目标，不表示整体优劣。例如 Cordis 不做持久 agent event store 并不是缺陷；它的抽象层本来就更低。

## 11. 建议的形式化修订

### 11.1 放弃不适配的经典 Lens law

建议把系统分成两个有明确类型的对象：

\[
view_L : (\Phi, E) \to C_L
\]

\[
handle_L : (W, Act, BeamToken) \to Result(Fact^*)
\]

其中：

- `Φ` 是已提交事实前缀；
- `E` 是不可变 LightPath epoch；
- `C_L` 是带 provenance 的 contribution 序列；
- `W` 是有界 PhotonWindow；
- `Fact*` 只能通过 host append gate 提交；
- `handle` 是 effectful handler，不伪装成纯 optic update。

`Surface` 定义为当前 epoch 有序 contribution 的 fold：

\[
Surface(\Phi,E)=fold([view_L(\Phi,E)\mid L\in Path(E)])
\]

### 11.2 可证明的卸载性质

令 `L` 在 `E` 中、但不在 `E+1` 中。可以主张：

#### 直接贡献消失

\[
\nexists c\in Surface(\Phi,E+1).\ origin(c)=(L,g_E)
\]

前提是：

1. Surface 只接受 `Path(E+1)` 中 Lens 当次计算的直接 contribution；
2. contribution provenance 不可伪造或至少由 host 覆盖；
3. 没有跨 epoch 未标记 cache；
4. 其他 Lens 不能冒充 `L,g_E`。

这个定理不禁止其他 Lens 合法引用历史中关于 `L` 的事实。

#### 新 Act 不再路由到旧代

\[
t>publish(E+1) \land start(a,t) \Rightarrow targetGeneration(a)\neq(L,g_E)
\]

前提是 Act target 绑定 epoch/generation，Beam ticket 只从当前 snapshot 发放，旧 guard 在发布时关闭新 admission。

#### 有界 afterglow

不能无条件证明旧代必然自然停止，只能定义策略：

\[
drain\ until\ deadline;\ then\ cancel;\ then\ process\ kill\ if\ isolated
\]

对同进程不合作 native code，C++ 无法安全强制终止单个线程并继续信任进程；应明确宿主重启或将不可信代码放到 worker 的策略。

### 11.3 安全属性与纯函数属性分开

建议文档区分：

- **Functional contract**：给定同一 `PhotonWindow + epoch`，`view` 贡献确定且无 host effect；
- **Capability enforcement**：OS/worker/host API 阻止绕过；
- **Lifecycle custody**：host 能枚举并关闭 mount 资源；
- **Cognitive visibility**：模型请求只来自可审计 Surface；
- **Historical integrity**：Photon 不被 API 或数据库内普通写操作修改。

这五项不能由一句“Lens 是纯函数”同时证明。

## 12. 建议的可复现实验

### 12.1 认知残留测试

不要首先调用 LLM。先做确定性 schema test：

1. mount Lens `P`，构造 Surface；
2. 记录所有 `origin=P,generation=g` contribution、tool schema、prompt fragment；
3. 发布移除 `P` 的新 epoch；
4. 用同一 Photon prefix 重建 Surface；
5. 断言新 Surface 无旧代直接 contribution；
6. 另行断言历史 Photon 仍存在且 hash chain 有效；
7. 添加一个合法 history/audit Lens，验证其仍可显示“P 曾经存在”，从而证明系统没有偷换成历史删除。

### 12.2 工具幻觉测试

LLM 幻觉不能由类型系统宣称为 0。应：

- 固定模型版本、system prompt、temperature、seed（若支持）；
- 至少数百个卸载前/卸载后任务；
- 区分“模型文本提到旧工具”和“运行时接受旧 target Act”；
- 运行时必须 100% 拒绝不存在或 generation 不匹配的 Act；
- 模型层报告 hallucination rate 与置信区间，不承诺数学 0%；
- 发布原始请求、响应、判分器和人工复核样本。

最重要的产品保证应是：**即使模型幻觉旧工具，结构化 admission 也不会执行它。** 这是可由运行时强制的安全属性。

### 12.3 卸载延迟测试

分别测量：

- candidate validate 时间；
- atomic publish 时间；
- 新请求不再看见旧 Lens 的时间；
- cooperative drain 时间；
- forced worker termination 时间；
- 资源 ledger 归零时间。

报告 P50/P95/P99/max，并按以下场景拆分：idle、CPU busy、blocked I/O、hung worker、large event history、多个并发 ray。`publish` 的微秒级不能被表述成“所有资源已拆卸”的总延迟。

### 12.4 内存测试

至少区分：

- process RSS/private bytes；
- heap live allocations；
- SQLite page cache；
- model/runtime 进程；
- worker 子进程；
- memory-mapped files；
- 长会话 Photon 数据量。

对 append-only 系统，“长期总存储保持常数”本来就不成立；合理目标应是有界工作集、可归档历史和无每次换代的不可达资源增长。

### 12.5 公平对照

与 Cordis 或 DSH 对照时，应实现同一个最小 workload：

- 一个 prompt contribution；
- 一个 calculator tool；
- 一个 listener/timer；
- 一个可取消 worker；
- 一次 provider replacement；
- 一次卸载后旧工具调用。

分别测认知 Surface、tracked resource、untracked side effect、publish/drain latency 和开发者必须编写的 lifecycle code。不要拿 VS Code 完整进程 RSS 与一个最小 C++ 示例比较。

## 13. 建议路线图

### P0：让主张与事实对齐

1. 把 RCLD 文档改为 whitepaper/research agenda，删除“已严格证明”“绝对零幻觉”“全面代际超越”等表述；
2. 以 `DESIGN.md` 的“未来零新增贡献”取代“历史零 token 残留”；
3. 删除 runtime 对 Calculator 的强制插入；
4. 实现最小可用 MountGuard/HostedTasks/HostedIO/Secrets ledger；
5. 发布 reproducible benchmark harness、原始数据和 build manifest；
6. 让 `IMPLEMENTATION-REPORT.md` 自动记录 commit、build type、OS 和真实测试计数。

### P1：补齐工程保障

1. 扩充 dark lane：history replay、determinism、synthetic Act、timeout/cancel、resource delta、worker crash；
2. 对所有 model request 强制 `epoch + surface hash + contribution provenance`；
3. 对所有 Act 强制 `target lens id + generation + epoch + policy decision hash`；
4. 为旧 epoch 增加“拒绝新 ticket”的明确原子状态；
5. 做 Linux/macOS 构建与 credential/envelope backend；
6. 在可用环境完成 Wasmtime 与 Docker live tests；
7. 增加 property/fuzz/crash-recovery/concurrency sanitizer 测试。

### P2：再谈范式与论文

1. 用小型 typed calculus 重写，而非为所有产品概念各造一条推导规则；
2. 只证明直接 contribution absence、epoch routing safety、append integrity 等可实现性质；
3. 把外部 emission、compensation、untrusted native code 列为显式边界；
4. 用 mechanized model 或 property-based executable model 验证 epoch publication/afterglow；
5. 与 Cordis/DSH 做等价 workload 的公开复现实验，再讨论范式优势。

## 14. 采用建议

### 适合采用的场景

- 需要长生命周期运行、动态启停工具的 agent host；
- 对模型上下文来源、工具调用和副作用审计要求高；
- 愿意使用结构化 schema 和 append-only event model；
- 需要 C++ 宿主以及 Node/Python worker 混合扩展；
- 可以接受当前处于高级原型阶段，并继续投资平台工程。

### 需要谨慎的场景

- 把“卸载”理解为外部现实自动回滚；
- 需要立即支持任意不可信 native plugin；
- 依赖庞大开放服务图和第三方插件生态；
- 要求 Linux/macOS/WASM/container/secret backend 已经全部生产验收；
- 需要用论文当前形式化结果作为安全或正确性证明。

### 推荐的实际策略

若继续发展 `tokmon-n`，建议保留并强化以下内核：

- append-only Photon + schema/provenance/hash；
- immutable LightPath epoch；
- Surface 每请求重建与 contribution provenance；
- structured Act + admission + audit；
- worker/process isolation；
- bounded afterglow。

同时弱化以下内容：

- 把所有子系统强行称为同一种数学 Lens；
- 把经典 lens laws 套在 event append handler 上；
- “纯函数自动解决所有资源和安全问题”；
- 未经实验的绝对性能与零幻觉承诺；
- 用固定 20 个神话命名替代可扩展 capability taxonomy。

## 15. 最终结论

从相对客观的角度，RCLD 的价值不在“发现了一个前所未有、数学上碾压 Cordis 的终极范式”，而在于把几类成熟思想针对 AI agent 重新组合成一个统一工程契约：

- event-sourced facts；
- deterministic/traceable projections；
- current-epoch capability surface；
- structured and gated effects；
- immutable reconfiguration；
- lifecycle drain and process isolation。

这个组合是有意义的，尤其是“认知拆卸”和“现实历史不伪回滚”的区分。它比传统插件 registry 更贴合 LLM 工具系统，也比仅依赖 disposer 更能说明不可逆外部世界。

但当前论文把一个优秀的工程直觉包装成了超出证据的形式化与实验结论。它的 Lens 公理和 append semantics 冲突，零残留证明忽略跨 Lens 历史投影，无死锁/合流性遗漏真实 I/O 和非确定性前提，benchmark 又不可复现。以研究标准衡量，尚不能接受其“已证明”的核心强结论。

`tokmon-n` 本身则呈现相反特征：**代码比论文诚实，也比论文更有价值。** 当前实现已经有真实 SQLite 不变性、hash chain、epoch 快照、Act audit、C ABI hot swap、多语言 worker、Windows Job Object 和广泛现实边界测试；这使它有资格被称为一个可信的高级原型。与此同时，MountGuard/统一资源 custody 未完整落地、同进程 purity 无强制隔离、dark lane 较浅、Calculator 被硬编码、跨平台和可选后端未完成、极限指标无证据，因此还不应称为工业级完成品。

最合理的总体定位是：

> **RCLD 是一个值得继续研究和工程化的 agent runtime 架构假说；`tokmon-n` 是该假说目前相当有分量、但仍处于 pre-production 阶段的实现。Cordis 在通用时空组合理论和 tracked effect lifecycle 上更成熟，deepseek-harness 在产品生态和工程规模上更成熟；Tokmon 的机会在于把持久因果事实、模型可见面、结构化作用与原生隔离做成真正可验证的一体化边界。**

---

## 附录 A：关键证据定位

| 主题 | 文件与位置 |
|---|---|
| 规范优先级、C++20、非目标 | `docs/DESIGN.md:15-22, 56-71, 114-126` |
| “无状态”的工程定义 | `docs/DESIGN.md:213-221` |
| 工程七定律 | `docs/DESIGN.md:203-211` |
| MountGuard 设计 | `docs/DESIGN.md:502-599` |
| “零新增贡献”语义 | `docs/DESIGN.md:1544-1566` |
| RCLD 强主张 | `docs/detachable-refractive-causal-lenses-paper.zh.md:13-20` |
| RCLD Lens 定义与 laws | 同文件 `:184-228` |
| RCLD 四个定理 | 同文件 `:268-289` |
| 论文示例中的模拟模型 | 同文件 `:441-604`，尤其 `:548` |
| PhotonStore trigger/hash | `nyxia/storage/photon_store.cpp:115-204, 269-280` |
| Photon append-only 测试 | `tests/unit/core_tests.cpp:137-165` |
| SDK Host/ILens | `sdk/cpp/include/tokmon/lens.hpp:57-104` |
| LightPath publish | `nyxia/light_path/light_path.cpp:29-117` |
| Act audit 和执行 | `nyxia/engine/ray_tracing_engine.cpp:70-171` |
| natural darkness | 同文件 `:174-215` |
| dark lane / Calculator / afterglow | `nyxia/runtime/runtime.cpp:604-730` |
| Worker Job Object/stop | `nyxia/worker/worker_lens_proxy.cpp:293-311, 456-462` |
| 当前实现限制 | `docs/IMPLEMENTATION-REPORT.md:204-209` |
| Cordis disposer / reactive epoch | `cordis/packages/core/src/fiber.ts:275-339, 385-457` |
| DSH append-only/model-visible | `deepseek-harness/docs/architecture.md:9-13, 43-102` |
| DSH session event log | `deepseek-harness/packages/core/session/src/index.ts:418-694` |

Cordis 论文页码索引：第 9 页开始定义 revertible effect 与 inverse；第 67-68 页讨论 system boundary、acquisition/emission、withholding 与 compensation；第 69 页讨论 capability 与恶意组件隔离；第 70 页讨论跨语言装卸限制；第 71 页讨论 mutual dependency 与组件粒度；第 72 页讨论 dependency typing/versioning。

## 附录 B：审查时使用的术语纪律

- **已实现**：在当前源码中有可定位逻辑，并有相应测试或可运行证据。
- **部分实现**：存在主路径，但未达到规范描述的边界或覆盖。
- **已设计**：规范中有接口/流程，当前源码没有完整对应。
- **已证明**：需要封闭定义、明确假设和可审查推导；代码测试通过不等于数学证明。
- **已验证**：需要给出环境、命令、版本和结果；单平台验证不外推为全平台。
- **零新增贡献**：新 epoch 不接受卸载 Lens 的直接新 contribution，不表示历史消失。
- **零幻觉安全**：更可实现的含义是旧 target Act 必被 admission 拒绝，而不是模型永远不生成旧工具名。
