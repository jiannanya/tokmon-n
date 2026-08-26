# `tokmon-n` 波前镜组与折射场演算设计

> 文档类型：已实现架构设计 / SDK、运行时与组合模型规范
> 编写日期：2026-08-26  
> 适用范围：Nyxia、C++ SDK、C ABI、多语言 Worker、正式 Lens、LightPath 配置与开发工具  
> 上位约束：[DESIGN.md](DESIGN.md)  
> 对照方案：[tokmon-n-bounded-synchronous-and-causal-asynchronous-lens-collaboration.zh.md](tokmon-n-bounded-synchronous-and-causal-asynchronous-lens-collaboration.zh.md)  
> 配套路线图：[tokmon-n-rcld-improvement-roadmap.zh.md](tokmon-n-rcld-improvement-roadmap.zh.md)

> 实现状态说明：BeatBoard/SOQ 对照方案从未实现。本文方案已作为当前架构一次性落地；C++、C ABI、Worker、官方 Lens、配置与测试均直接使用 Wavefront 接口，不存在需要兼容的 SOQ runtime 或旧 Lens 接口。

## 1. 执行摘要

`tokmon-n` 的 `LightPathSnapshot` 现在同时固定 Lens generation 与已编译的 `OpticalAssemblySnapshot`。每个 Lens 从 `OpticalInput` 读取冻结 Photon 与局部入射场，产生 FieldCell；现实动作通过 `Act → admission → target Lens.refract() → Photon` 完成。未实现的有限同步与因果异步协同草案曾提出 BeatBoard 与 SOQ，现仅作为设计对照。

如果实现，BeatBoard/SOQ 能以较低风险解决同拍能力复用，但其协同原语仍然是宿主中介的能力调用：

```text
Consumer Lens → OpticalContext.query() → Nyxia → Provider query handler
```

这意味着按该草案实现后，系统除了 `view/refract` Lens 之外，还会出现 query provider、能力注册表和共享折光板。它满足“No direct object call”，却没有完全满足“所有业务变换与组合自身也都是 Lens”。

本文提出另一种设计：**Wavefront Optical Assembly（波前镜组）与 Refractive Field Calculus（折射场演算）**。

核心思想如下：

1. `PhotonWindow` 是本拍冻结的因果光源；
2. Lens 不查询其他 Lens，只接收类型化入射波前并产生不可变出射波前；
3. 串联、分束、棱镜选择、合束、光阑、延迟和谐振都是结构 Lens；
4. 原子 Lens 与复合镜组具有同一种外部形状，镜组可以继续嵌套、挂载、替换和拔出；
5. 普通无环镜组按确定拓扑传播，真实同步循环只能进入受约束的有界谐振区；
6. 任何非纯计算、外部 I/O、规范状态变化或不可证明收敛的反馈都必须经过因果延迟，以 Act/Photon 进入下一拍；
7. Nyxia 只充当光具座、镜组编译器、传播调度器和安全边界，不向 Lens 暴露 provider lookup 或通用 query API。

一句话概括：

> Lens 不调用 Lens；光只是在镜组中传播。协同是波前相遇，循环是受控谐振，现实变化是因果折射。

本文已经替换旧 `Linear Fold` 工程约束，同时保留 Append Only、Pure View、No Side Channel、Act Before Reality、Patterned Refraction 和 Darkness Means Stop。顶层仍只有一个权威光路快照，镜片内部允许封装且可证明的复合光学空间。

## 2. 问题定义

### 2.1 当前线性折叠的组合上限

当前工程模型近似为：

```text
PhotonWindow
    ├─ Lens A.view(PhotonWindow) → contribution A
    ├─ Lens B.view(PhotonWindow) → contribution B
    └─ Lens C.view(PhotonWindow) → contribution C
                              ↓
                     deterministic merge
                              ↓
                           Surface
```

Lens 之间没有入射/出射关系。`LightPath` 的顺序主要决定稳定合并顺序，而不是“后一个 Lens 接收前一个 Lens 折射后的光”。因此它更像在同一事实快照上运行多个投影器，而不是真正的串联镜组。

这会造成以下问题：

1. 后游 Lens 无法自然消费上游 Lens 本拍派生的数据；
2. 复合能力只能隐含在 Surface channel 的约定或某个大 Lens 中；
3. 参数化能力复用需要额外 query/RPC 抽象；
4. 平行协作、局部子空间、显式合束和反馈没有统一组合代数；
5. 常用多镜组合不能封装成一片具有相同契约的新 Lens；
6. `LightPath` 在配置层可组合，但数据流在语义层仍是全局、平坦和弱连接的。

### 2.2 BeatBoard/SOQ 拟解决什么

BeatBoard/SOQ 草案提出 `Derive → Freeze → Coordinate`：

- Provider 在 Derive 阶段建立 Frozen State；
- Consumer 在 Coordinate 阶段通过 capability name 和结构化参数查询；
- Nyxia 负责解析 Provider、校验 schema、限制预算和禁止嵌套调用；
- 现实动作仍通过 Act/Photon。

该方案的优点是：

- 对现有 `ILens` 改动有限；
- 同拍读写边界清晰；
- 适合 AST、上下文选择和工具 schema 校验等点查询；
- 可用固定深度调用避免递归、死锁和生命周期泄漏；
- 易于跨进程映射为有界 request/response IPC。

### 2.3 BeatBoard/SOQ 设计仍未解决什么

从“Everything is a Lens”的更强解释看，它仍有五个结构性缺口：

1. **协同不是 Lens**：`query()` 是 Lens 之外的第二种业务计算形状；
2. **空间是全局黑板**：所有 Frozen State 平铺在同一 BeatBoard，而不是通过局部边界形成可嵌套空间；
3. **组合不闭合**：A 查询 B 的结果不能自然封装成一个具有相同输入输出契约的复合 Lens；
4. **依赖是调用边**：capability 解析后仍形成 Consumer → Provider 的运行时调用关系；
5. **循环只被禁止**：同步循环没有正面的、可证明终止的表达方式。

### 2.4 不可能同时保留的四个条件

以下四个条件无法同时成立：

1. 只有严格直线光路；
2. 每拍只传播一次；
3. 不存在共享派生空间；
4. 任意多个 Lens 可以同拍对称协同。

不同设计必须放松至少一项：

| 设计 | 放松的条件 |
|---|---|
| BeatBoard/SOQ | 引入共享冻结空间和受控调用 |
| 逐镜渐进折射 | 对称协同退化为有序上游到下游 |
| 波前镜组 | 将严格直线扩展为封装的 DAG/组合树 |
| 谐振波前 | 允许拍内有界多轮传播 |
| Act/Photon | 将协同延迟到下一拍 |

本文选择“波前镜组为常态、因果延迟为反馈默认、有限谐振为严格例外”。

## 3. 设计目标与非目标

### 3.1 设计目标

本设计必须做到：

1. 让 Lens 的同拍协同只通过入射/出射波前完成；
2. 让串联、并行、路由、合并、预算、反馈和封装都具有 Lens 形状；
3. 让复合镜组对外暴露有限、类型化的边界端口，并可以继续作为 Lens 参与组合；
4. 保留 Photon 作为唯一规范事实源，Wavefront 不成为第二套历史或恢复依据；
5. 保留 Act 作为现实副作用的唯一结构化意图；
6. 允许 Textus、Enso、Techor、Janus 等在同拍形成明确数据流；
7. 允许参数化只读计算被表达为 request/index/answer Lens 链，而不是同步 RPC；
8. 对无环镜组提供确定、可并行、可缓存的传播；
9. 对真正的纯计算循环提供可选、有界、可证明的固定点语义；
10. 对非纯或不收敛循环强制插入跨拍因果延迟；
11. 保持 generation、epoch、schema、权限、预算、provenance 和热拔插安全；
12. 支持 C++、C ABI、Worker 和 WASM 共享同一组合语义；
13. 让 C++、C ABI、Node.js、CPython 与官方 Lens 直接共享同一套已实现接口。

### 3.2 非目标

本设计不试图：

- 使用真实几何光学的浮点角度、焦距或折射率模拟业务逻辑；
- 允许 Lens 持有或调用其他 Lens 对象；
- 允许任意运行时动态连线绕过 mount-time 验证；
- 将拍内 Wavefront 持久化为完整 Photon 历史；
- 让纯 Lens 直接访问文件、网络、模型、进程、Secret 或系统时钟；
- 自动求解任意非单调程序或无限递归；
- 用固定点计算替代工作流、审批、重试、恢复和幂等；
- 让业务 Lens 感知线程、任务队列、Worker endpoint 或 provider generation 对象；
- 提供任何旧 Lens ABI、共享 Surface 或 Board/SOQ 兼容层。

## 4. 设计公理

### 4.1 唯光不灭

已经发生并被系统承认的事实只存在于只追加的因果 Photon 流中。Wavefront、Surface、缓存、索引和谐振中间态都可以删除并从 Photon 与当前镜组重建。

### 4.2 构件即透镜

所有业务语义变换必须采用统一 Lens 形状：读取已声明的入射端口，产生已声明的出射端口；现实折射只能接受已准入 Act 并追加 Photon。

以下对象也必须作为 Lens 或镜组结构 Lens 表达：

- 分束；
- 类型路由；
- 多源合并；
- 预算裁剪；
- 策略过滤；
- query request/answer 变换；
- Surface 投影；
- Act proposal 聚合；
- 因果延迟；
- 有界谐振。

### 4.3 组合必须闭合

如果 `L1` 和 `L2` 是 Lens，则它们的合法串联、并行、选择、合束或封装结果也必须是 Lens：

```text
Lens × Lens → Lens
```

复合 Lens 不能要求外部调用者了解其内部成员、provider ID、执行顺序或缓存对象。

### 4.4 局部折射，不做横向调用

Lens 只能依据：

- 当前冻结 `PhotonWindow`；
- 当前端口收到的不可变入射场；
- 签名 artifact 内的配置；
- Nyxia 提供的确定性、无业务语义的预算与取消信息。

Lens 不获得：

- `LensRegistry`；
- Provider 指针；
- 通用 service locator；
- `OpticalContext::query()`；
- 可写 PhotonStore；
- 另一个 Lens 的生命周期句柄。

### 4.5 反馈必须受保护

每一个组合环必须满足以下二选一：

1. 环中包含 `CausalDelayLens`，把反馈变成明确 Act/Photon，下一拍再观察；
2. 整个强连通子空间被声明为 `ResonatorLens`，且满足纯、单调、有限合并、严格预算和确定终止条件。

未受保护的环在候选镜组发布阶段被拒绝。

### 4.6 光学隐喻只承载有用语义

本文的光学映射为：

| 光学概念 | 工程含义 |
|---|---|
| 光源 | 冻结 PhotonWindow 的源投影 |
| 波长/频带 | schema 化的 `BandId` |
| 入射端口 | Lens 声明消费的 typed input |
| 出射端口 | Lens 声明产生的 typed output |
| 折射 | 纯派生变换或已准入现实动作 |
| 分束 | 一个输出连接多个独立分支 |
| 干涉/合束 | 依据显式代数合并多个来源 |
| 光阑 | token、byte、数量、trust、sensitivity 预算 |
| 光程 | 稳定、可追踪的组合路径 |
| 延迟线 | 跨 Act/Photon 的下一拍反馈 |
| 谐振腔 | 纯单调子空间的有限固定点传播 |
| 像平面 | 最终 Model/UI/CLI/Act/Diagnostic Surface |

不把物理焦距、角度或斯涅尔定律直接编码成业务浮点参数。Tokmon 需要的是局部接口匹配、传播方向、合并守恒和反馈约束，而不是装饰性的物理模拟。

## 5. 核心对象

### 5.1 PhotonWindow：冻结因果光源

`PhotonWindow` 继续表示某一 Ray、某一拍读取到的已提交 Photon 前缀。它具有：

- 明确的 ray/stream；
- `head/tail sequence`；
- LightPath/Assembly epoch；
- 完整性 hash；
- 确定的 schema 解码；
- 只读访问；
- 可分页或通过 host-owned projection cache 加速。

同一拍传播期间，PhotonWindow 不变化。传播产生的 Wavefront 不追加回 PhotonWindow。

### 5.2 OpticalBand：类型化光谱频带

`OpticalBand` 是拍内派生值的语义类型，不是 provider 名称：

```cpp
struct OpticalBandSpec {
  BandId id;                       // e.g. model.context
  SchemaId schema;                 // e.g. tokmon.model.context.v2
  Cardinality cardinality;
  MergeLaw merge;
  Sensitivity sensitivity;
  TrustRequirement trust;
  Durability durability;           // must be transient for Wavefront
  std::size_t max_cell_bytes;
  std::size_t max_cells;
};
```

典型 Band：

```text
model.messages
model.context.candidate
model.tools
model.catalog
syntax.index
syntax.request
syntax.answer
completion.intent
completion.candidate
policy.observation
act.proposal
surface.model.fragment
surface.ui.fragment
diagnostic.optical
```

Band 表达“什么光可以连接”，而不是“调用哪个 Lens”。

### 5.3 OpticalPort：镜片边界

每个 Lens 声明输入和输出端口：

```cpp
struct OpticalPortSpec {
  PortName name;
  Direction direction;
  BandId band;
  SchemaId schema;
  PortCardinality cardinality;
  Requirement requirement;
  MergeLaw accepted_merge;
  BudgetSpec budget;
};
```

端口名称属于 Lens 私有契约，Band/Schema 属于可组合公共契约。例如：

```yaml
inputs:
  - port: context_candidates
    band: model.context.candidate
    schema: tokmon.model.context-candidate.v2
    cardinality: many
    required: false

outputs:
  - port: selected_context
    band: model.context
    schema: tokmon.model.context.v2
    cardinality: one
```

连接合法性在候选镜组编译阶段验证，不在执行过程中临时猜测。

### 5.4 FieldCell：拍内最小派生单元

Wavefront 中的每个值封装为不可变 `FieldCell`：

```cpp
struct FieldCell {
  FieldCellId id;
  BandId band;
  SchemaId schema;
  Bytes payload;
  FieldProvenance provenance;
  TrustLabel trust;
  Sensitivity sensitivity;
  CostMeasure cost;
};
```

建议的稳定身份：

```text
FieldCellId = hash(
  beat_key
  + producer_lens_id
  + producer_generation
  + output_port
  + canonical_input_cell_ids
  + canonical_payload_hash
)
```

`FieldCellId` 不应包含线程调度顺序。对于谐振传播，也不应仅因 round 不同而产生不同身份，否则同一派生值无法幂等收敛。

### 5.5 Wavefront：拍内不可变派生光场

`Wavefront` 是按 Band 分组的不可变 FieldCell 集合：

```cpp
struct Wavefront {
  BeatKey beat;
  AssemblyHash assembly;
  PersistentMap<BandId, FieldSet> bands;
  Hash canonical_hash;
};
```

它具有以下语义：

- 只在当前 Beat 生命周期内有效；
- 不是事实，不进入 PhotonStore；
- 删除后不影响规范状态；
- 每次扩展返回新逻辑版本，底层可以结构共享；
- 所有合并都服从 Band 声明的 MergeLaw；
- 每个 Cell 保留完整生产与输入 provenance；
- Lens 只能看到实际连接到其输入端口的子集，而不是全局 Wavefront。

### 5.6 Surface：最终像平面

Surface 继续是本拍对外可消费的有界视界。但 Surface 不再由所有业务 Lens 直接写共享 Builder，而由一个或多个终端 `ProjectionLens` 从 Wavefront 投影：

```text
Wavefront
   ├─ ModelProjectionLens → ModelSurface
   ├─ UiProjectionLens → UiSurface
   ├─ CliProjectionLens → CliSurface
   ├─ ActProjectionLens → ActSurface
   └─ DiagnosticProjectionLens → DiagnosticSurface
```

这使 Surface Gate、预算和冲突决策本身也成为显式镜片，而不是隐藏在共享容器中的副作用。

### 5.7 OpticalAssembly：可嵌套光学空间

`OpticalAssembly<InputPorts, OutputPorts>` 是一片复合 Lens。它内部包含：

- 原子 Lens generation；
- 结构 Lens；
- 端口连接；
- 局部预算；
- 局部信任边界；
- 可选谐振区；
- 对外暴露的输入/输出端口。

外部只能观察 Assembly 的边界契约，不能直接依赖内部 Lens ID：

```text
PrimitiveLens ⊂ OpticalAssembly
OpticalAssembly is Lens
```

这构成空间可组合性的核心：一个 Agent、Prompt Weaver、代码智能子系统或策略管线都能封装成一片 Lens，再装入更大镜组。

## 6. 折射场组合代数

### 6.1 恒等镜 `identity`

恒等镜原样传递声明的输入 Band：

```text
I : A → A
I(a) = a
```

它用于可选插槽、兼容降级和组合定律测试。

应满足：

```text
L ∘ I = L
I ∘ L = L
```

### 6.2 串联 `compose`

如果 `L1 : A → B` 且 `L2 : B → C`，则：

```text
L2 ∘ L1 : A → C
```

运行语义：

```text
A → L1 → B → L2 → C
```

与当前所有 Lens 独立读取同一 PhotonWindow 不同，L2 明确接收入射 Band B。中间 B 可以保持为内部端口，不暴露到 Assembly 外部。

### 6.3 平行 `tensor`

如果 `L1 : A → B` 且 `L2 : C → D`，则：

```text
L1 ⊗ L2 : A × C → B × D
```

两个分支语义独立，可以并行执行；最终结果不依赖线程完成顺序。

### 6.4 分束 `split`

分束镜把同一输入连接到多个下游：

```text
          ┌→ L1
A → Split ├→ L2
          └→ L3
```

它不复制规范事实，只为多个分支建立同一不可变 FieldCell 的只读引用。大对象应使用结构共享或 host-owned beat handle。

### 6.5 棱镜选择 `prism`

棱镜按照 schema、tag、trust、sensitivity、语言、文件类型或明确策略将复合入射场分解到不同端口：

```text
MixedBand → Prism
              ├→ cpp
              ├→ python
              └→ fallback
```

路由条件必须：

- 在 manifest 中声明；
- 使用确定性输入；
- 不访问外部世界；
- 对重叠和未匹配情况给出明确策略；
- 产生可追踪的路由 provenance。

### 6.6 合束/干涉 `merge`

多源输出不能依赖“最后写入者胜出”或线程时序，必须声明合并代数。

推荐 MergeLaw：

| MergeLaw | 语义 | 是否适合谐振 |
|---|---|---|
| `set_union` | 按稳定 CellId 去重并集 | 是 |
| `map_union_unique` | key 唯一，冲突报错 | 是 |
| `priority_then_path` | priority、path index、CellId 全序 | 条件式 |
| `top_k` | 全序候选并集后确定性取前 K | 是，若比较器稳定 |
| `product` | 多个必需单值组成乘积 | DAG 内使用 |
| `optional_single` | 零或一，多个即冲突 | DAG 内使用 |
| `stable_concat` | 按拓扑路径稳定拼接 | 否，除非输入有限且只追加 |
| `custom_lattice` | schema 注册的 join | 需验证 |

用于谐振的 join 必须满足：

```text
结合律：a ⊔ (b ⊔ c) = (a ⊔ b) ⊔ c
交换律：a ⊔ b = b ⊔ a
幂等律：a ⊔ a = a
```

如果合并只对某个顺序确定而不满足交换律，它只能位于无环镜组中，由拓扑序决定结果。

### 6.7 光阑 `aperture`

光阑是一片有界选择 Lens，而不是共享显像容器内部的隐式截断：

```text
Candidates → Aperture(token/bytes/count/trust) → SelectedCandidates
```

光阑必须输出：

- 被选择值；
- 被丢弃数量；
- 选择规则版本；
- 预算消耗；
- 截断原因；
- 输入/输出 hash。

若完整候选本身不需要恢复，不提交 Photon；若最终选择改变未来规范行为，应把最小选择证据附入 Act provenance 或提交决策 Photon。

### 6.8 因果延迟 `causal_delay`

因果延迟把无法安全在本拍闭合的反馈转换为明确因果链：

```text
Wavefront band
    ↓
CausalDelayLens
    ↓ propose Act / fact intent
Act admission
    ↓
target.refract
    ↓
committed Photon
    ↓ next Beat
SourceProjectionLens
    ↓
new Wavefront band
```

因果延迟不能把任意 Wavefront 全量序列化进 Photon。它必须使用注册 schema，将拍内派生压缩为恢复、安全或未来行为所需的最小充分事实。

### 6.9 谐振 `resonator`

谐振镜组表示一个纯、单调、有限的同拍反馈子空间。设初始波前为 `W0`，内部 Lens 集合为 `R`：

```text
W(k+1) = W(k) ⊔ ⋁ { L(W(k)) | L ∈ R }
```

停止条件：

```text
canonical_hash(W(k+1)) == canonical_hash(W(k))
```

或没有新的 FieldCell delta。

谐振区必须声明：

```yaml
resonator:
  max_rounds: 8
  max_cells: 10000
  max_bytes: 16777216
  timeout_ms: 50
  on_non_convergence: fail
```

默认 `on_non_convergence` 必须是 `fail` 或显式降级分支，不能静默采用最后一轮结果。

### 6.10 封装 `assembly`

任意合法组合都可以隐藏内部端口并暴露边界端口：

```text
assembly PromptWeaverLens
  input: conversation, workspace_state
  output: model.surface.fragment, act.proposal
```

封装后仍满足：

- 可签名；
- 可版本化；
- 可 dark-lane 验证；
- 可原子换代；
- 可拔出；
- 可回放；
- 可作为另一个 Assembly 的子 Lens。

## 7. 传播执行模型

### 7.1 一拍的总体流程

```text
1. 捕获不可变 OpticalAssemblySnapshot
2. 读取冻结 PhotonWindow
3. SourceProjectionLens 产生初始 Wavefront
4. 按拓扑层传播所有无环区域
5. 对声明的 Resonator 区执行有界 delta 迭代
6. ProjectionLens 形成有界 Surface
7. ActProjectionLens 形成候选 Act
8. 确定性 arbitration 与 admission
9. 唯一目标 Lens.refract 执行现实折射
10. 结果提交为 Photon，后续拍重新成像
11. 无新 Act、无待完成 Beam 且无未消费输入时自然暗化
```

### 7.2 镜组编译

候选 Assembly 发布前，Nyxia 将声明式布局编译为不可变传播计划：

```cpp
struct OpticalAssemblySnapshot {
  MountEpoch epoch;
  AssemblyHash hash;
  std::vector<MountedOptic> optics;
  std::vector<CompiledConnection> connections;
  std::vector<PropagationLayer> dag_layers;
  std::vector<CompiledResonator> resonators;
  std::vector<ProjectionEndpoint> projections;
  std::vector<RefractionRoute> act_routes;
};
```

编译阶段至少执行：

1. 输入/输出 Band 与 Schema 匹配；
2. required port 完整性；
3. cardinality 和 merge law 兼容性；
4. trust/sensitivity 信息流检查；
5. 资源预算聚合；
6. generation 和 artifact hash 固定；
7. 环检测与 SCC 分类；
8. 未保护环拒绝；
9. Resonator merge law 合法性；
10. ActPattern 唯一目标与固定 admission 路线；
11. 对外暴露端口与内部端口隔离；
12. 确定性 path index 分配。

### 7.3 无环传播

对于 DAG 区域，按拓扑层执行：

```text
Layer 0: Source/independent derive lenses
Barrier + deterministic merge
Layer 1: consumers of Layer 0
Barrier + deterministic merge
...
Layer N: projection lenses
```

同层 Lens 在语义上并行。实现可以串行或并发，但必须按照预编译的稳定顺序合并 FieldCell。

### 7.4 Delta 传播

Nyxia 不必每轮把完整 Wavefront 复制给每个 Lens。每个输入端口维护已消费 CellId 集合：

```text
new_delta(port) = connected_cells(port) - consumed_cells(port)
```

Lens manifest 声明其触发策略：

| 策略 | 语义 |
|---|---|
| `once_when_ready` | 所有 required input ready 后执行一次 |
| `on_delta` | 每次出现新 delta 时增量执行 |
| `on_seal` | 上游端口封闭后执行一次全量计算 |
| `per_key_join` | 相同业务 key 的输入齐备后执行 |

默认使用 `once_when_ready` 或 `on_seal`。只有明确实现增量幂等契约的 Lens 才能使用 `on_delta`。

### 7.5 传播伪代码

```cpp
Result<SealedBeat> OpticalEngine::view(const RayId& ray) {
  auto assembly = nyxia_.optical_assembly();
  auto window = photon_store_.read_window(ray);
  BeatRuntime beat{assembly, window, limits_};

  beat.inject(source_projection_.view(window));

  for (const auto& layer : assembly->dag_layers) {
    auto results = execute_layer(layer, beat.ready_inputs(layer));
    beat.merge_deterministically(results);
    if (beat.budget_exhausted()) {
      return fail_view("optical_budget_exhausted");
    }
  }

  for (const auto& resonator : assembly->resonators) {
    TOKMON_TRY(run_resonator(resonator, beat));
  }

  auto surfaces = project_and_seal(beat);
  auto proposals = arbitrate(surfaces.act_proposals());
  return SealedBeat{std::move(surfaces), std::move(proposals), beat.trace()};
}
```

谐振执行：

```cpp
Result<void> OpticalEngine::run_resonator(
    const CompiledResonator& resonator,
    BeatRuntime& beat) {
  for (std::uint32_t round = 0; round < resonator.max_rounds; ++round) {
    const Hash before = beat.subspace_hash(resonator);
    auto deltas = execute_resonator_round(resonator, beat, round);
    beat.join_lattice_deltas(resonator, deltas);

    if (beat.subspace_hash(resonator) == before) {
      beat.mark_converged(resonator, round);
      return {};
    }
    if (beat.exceeds(resonator.budget)) {
      return unexpected(Error::optical_budget_exceeded(resonator.id));
    }
  }
  return unexpected(Error::resonator_not_converged(resonator.id));
}
```

### 7.6 确定性

相同结果至少绑定：

```text
BeatKey =
  ray
  + input_prefix_hash
  + assembly_epoch
  + assembly_hash
  + artifact/config hashes
```

确定性要求：

- 输入 CBOR canonical encoding；
- stable port and path indices；
- merge law 版本固定；
- 相同 FieldCell 语义产生相同 ID；
- 禁止读取未冻结时钟、随机数或全局可变状态；
- 同层完成顺序不影响输出；
- 谐振稳定性由 delta/hash 判断，不由 wall-clock 猜测；
- timeout 只决定失败，不决定采用哪个部分结果；
- 回放能够定位每个输出的完整光程。

### 7.7 缓存

拍内缓存键建议为：

```text
CacheKey =
  beat_key
  + lens_id
  + generation
  + input_port_hashes
  + config_hash
  + implementation_hash
```

跨拍缓存只能用于可丢弃派生状态，并必须绑定：

- Photon prefix/tail；
- Assembly epoch/hash；
- Lens artifact/config hash；
- schema/codec version；
- 外部资源的已提交版本事实。

删除全部缓存后必须得到相同 Wavefront 和 Surface。

## 8. 参数化能力的 Lens 化

### 8.1 Query Handler 升格为 Query Lens

BeatBoard/SOQ 对照草案中的：

```text
FrozenState × Request → Response
```

本身就是一个纯变换。本文把它升格为普通 Lens：

```text
QueryLens : IndexBand × RequestBand → ResponseBand
```

它不注册回调，不被 Consumer 调用，也不接收通用查询上下文。

### 8.2 请求相关性

请求 Cell 使用确定性相关 ID：

```text
request_id = hash(
  consumer_generation
  + request_band
  + canonical_parameters
  + causal_input_cell_ids
)
```

响应必须携带：

- `request_id`；
- provider Lens generation；
- index/input provenance；
- response schema；
- canonical response hash；
- cost/diagnostic metadata。

Consumer 通过普通端口接收匹配响应，不轮询、不等待、不解析自然语言。

### 8.3 多 Provider

多 Provider 不由 Consumer 逐一调用。镜组编译器根据连接声明形成：

```text
request → Splitter → Provider A ─┐
                   → Provider B ─┼→ InterferenceLens → response
                   → Provider C ─┘
```

选择规则属于结构 Lens：

- `first_by_priority`；
- `all`；
- `race_deterministic` 不允许依赖实际完成时间；
- `quorum`；
- `top_k`；
- `fallback_on_error`，顺序由 Assembly 固定；
- `exclusive`，多个 Provider 时发布失败。

### 8.4 大型冻结状态

AST、符号索引、向量索引等可以通过 `DerivedFieldHandle` 在同进程镜组中零拷贝传递：

```cpp
struct DerivedFieldHandle {
  BeatKey beat;
  LensGeneration owner;
  TypeId type;
  Hash content_hash;
  HostHandle handle;
};
```

限制：

- handle 只在当前 Beat/Assembly lease 内有效；
- Consumer 不能保存到下一拍；
- Consumer 不能向 handle 指向对象写入；
- 跨 Worker 边界必须转换为有界 CBOR、共享内存快照或 artifact reference；
- handle 不得进入 Photon、普通日志或 Surface；
- Provider 卸载时由当前拍 lease 保证已开始传播安全完成。

## 9. 代表性镜组

### 9.1 Prompt Weaver 与 Janus

```text
                                ┌─ Textus ───── model.messages ──────┐
PhotonSource → SpectralSplitter ├─ Enso ─────── context.candidate ───┤
                                ├─ Techor ───── model.tools ─────────┤
                                └─ RheaCatalog ─ model.catalog ──────┘
                                                                       ↓
                                                           ContextApertureLens
                                                                       ↓
                                                          ModelInterferenceLens
                                                                       ↓
                                                                  Janus Lens
                                                                       ↓
                                                               model.call proposal
                                                                       ↓
                                                           ActProjection/Admission
                                                                       ↓
                                                                 Rhea.refract
```

语义边界：

- Textus、Enso、Techor、RheaCatalog 不互相调用；
- ContextApertureLens 显式负责 token/trust/relevance 预算；
- ModelInterferenceLens 显式组成最终 ModelSurface fragment；
- Janus 只消费稳定的组合结果；
- 真正模型调用仍是 `model.call` Act；
- 模型响应以 Photon 进入下一拍。

整个结构可以封装为：

```text
PromptWeaverLens : AgentInputBand → ModelCallProposalBand
```

### 9.2 Syntax 与 Completion

```text
PhotonSource ─→ WorkspaceSnapshotProjection ─→ SyntaxIndexer ─→ syntax.index
                                                                  │
PhotonSource ─→ CursorIntentLens ─────────────→ syntax.request ────┤
                                                                  ▼
                                                      SyntaxAtPositionLens
                                                                  │
                                                           syntax.answer
                                                                  │
PhotonSource ─→ CompletionIntentLens ──────────────────────────────┤
                                                                  ▼
                                                          CompletionLens
                                                                  │
                                                      completion.candidate
                                                                  ▼
                                                        CompletionAperture
```

同拍查询由数据流完成。`SyntaxAtPositionLens` 可以是 Syntax artifact 内的子 Lens，但对外只通过端口参与组合。

### 9.3 多检索器合束

```text
search.request
    ↓ Splitter
    ├─ SymbolSearchLens ───────┐
    ├─ LexicalSearchLens ──────┼→ RankedInterferenceLens → context.candidate
    └─ FrozenVectorSearchLens ─┘
```

所有检索只使用当前已冻结索引。需要刷新索引时：

```text
workspace.reindex proposal
→ Act admission
→ Indexer.refract
→ index.updated Photon
→ 下一拍重建 Frozen index
```

### 9.4 策略折射

候选 Act 可以经过显式策略镜组：

```text
act.proposal
  → SchemaValidationLens
  → RiskClassificationLens
  → PolicyPolarizerLens
  → ApprovalRequirementLens
  → AdmissionProjectionLens
  → target.refract
```

前四步只产生纯派生判断。真正“允许该 Act 执行”的准入收据仍由 Nyxia 固定 gate 绑定 policy hash、approval Photon、epoch、generation 和 deadline，业务 Lens 的预判不能替代宿主权威。

### 9.5 有界互相修正

假设 A 的候选依赖 B 的约束，B 的候选又依赖 A 的观察：

```text
       ┌──────── A Lens ←───────┐
input ─┤                         ├→ stable.result
       └──────── B Lens ────────→┘
```

仅当 A、B 都以单调 delta 添加约束/候选，且 merge law 是有限半格时，才允许封装为 Resonator：

```text
round 0: A0, B0
round 1: A 根据 B0 追加 A1；B 根据 A0 追加 B1
round 2: 无新 Cell，收敛
```

如果 A 会撤销 B 的结果、依赖随机模型、访问文件或改变现实状态，必须拆成 Coordinator、显式工作流或跨拍因果延迟。

## 10. Manifest 与布局配置

### 10.1 Wavefront Lens Manifest

```yaml
api: tokmon.lens/wavefront
id: org.tokmon.lens.syntax-at-position
display_name: Syntax At Position
version: 1.0.0
abi: { major: 2, minor: 0 }
runtime: { kind: wasm }
trust: t1
stateless: true

inputs:
  - port: index
    band: syntax.index
    schema: tokmon.syntax.index-handle.v1
    cardinality: one
    required: true
  - port: request
    band: syntax.request
    schema: tokmon.syntax.at-position.request.v1
    cardinality: many
    required: true

outputs:
  - port: answer
    band: syntax.answer
    schema: tokmon.syntax.at-position.response.v1
    merge: map_union_unique
    max_cells: 1024
    max_cell_bytes: 262144

trigger: per_key_join
monotone: true
light_permissions: []
resources:
  memory_mb: 32
  output_bytes: 1048576
  deadline_ms: 20
```

纯 Query Lens 的 `permissions` 必须为空或只包含确定性 host compute primitive。它不能请求 filesystem/network/process/model/secret 权限。

### 10.2 Assembly 配置

`.tokmon/light-path.yaml` 直接声明 mounted Lens、显式连接、可选 Resonator 与总预算：

```yaml
api: tokmon.light-path/wavefront
lenses:
  - { id: org.tokmon.lens.textus, artifact: builtin:textus,
      enabled: true, runtime: in_process }
  - { id: org.tokmon.lens.context-aperture, artifact: builtin:context-aperture,
      enabled: true, runtime: in_process }
assembly:
  id: org.tokmon.assembly.default-agent
  autowire_unique: false
  connections:
    - from: { lens: org.tokmon.lens.textus, port: context_candidates }
      to: { lens: org.tokmon.lens.context-aperture, port: candidates }
  resonators: []
  budget:
    max_cells: 16384
    max_bytes: 16777216
    max_cell_bytes: 1048576
    max_lens_executions: 4096
    max_rounds: 8
    deadline_ms: 5000
```

正式 schema 可以比该示例更紧凑，但必须保留显式端口、连接、合并和边界暴露，不能退化为在 Consumer 代码中按字符串遍历全局 Band。

### 10.3 自动接线

可以提供语法糖自动连接唯一匹配的 Band/Schema，但编译结果必须完全物化并计算 hash：

```yaml
autowire_unique: true
```

规则：

- 恰好一个兼容输出时可连接；
- 零个且 required 时失败；
- 多个时必须显式声明 splitter/merge/selection；
- 自动接线结果写入候选 Assembly evidence；
- 运行时不重新选择 Provider；
- generation 更替只能通过新 epoch 发布。

### 10.4 可选能力

可选输入必须声明 Lens 化的降级路径：

```text
OptionalInput ─ present ─→ MainLens
              └ absent ─→ DefaultLens
```

缺失端口不能返回空指针或在运行时动态 lookup。`DefaultLens` 可以产生空集合、保守策略或诊断 Band。

## 11. C++ SDK 设计

### 11.1 Lens 形状

```cpp
class WavefrontBuilder {
public:
  Result<FieldCellId> emit(
      PortName output,
      std::string key,
      cbor::Value value,
      std::span<const FieldCellId> caused_by = {},
      std::int32_t priority = 0);
  Result<void> propose(
      Act act,
      std::span<const FieldCellId> caused_by = {});
};

class ILens {
public:
  virtual ~ILens() = default;

  [[nodiscard]] virtual const LensManifest& manifest() const noexcept = 0;

  virtual Result<void> view(
      const OpticalInput& input,
      WavefrontBuilder& outgoing) = 0;

  virtual Result<RefractionResult> refract(
      const PhotonWindow& photons,
      const Act& act,
      RefractionBeam& beam) = 0;
};
```

保留 `view/refract` 两种方向，不增加通用 `query()`。

### 11.2 IncidentWave

```cpp
class IncidentWave {
public:
  const std::vector<FieldCell>& cells(PortName port) const noexcept;
  const FieldCell* one(PortName port) const noexcept;
  bool connected(PortName port) const noexcept;
  bool sealed(PortName port) const noexcept;
};
```

`IncidentWave` 只能访问已编译连接送达的端口。不存在 `get_all("arbitrary.band")`，从类型层面维持局部空间边界。

### 11.3 WavefrontBuilder 强制规则

Builder 由宿主创建并绑定当前 Lens generation，自动强制：

- 输出 port 已声明；
- schema 编码与大小合法；
- Cell 数量和总字节不超预算；
- provenance 输入只能引用当前可见 Cell 或 Photon；
- trust/sensitivity 不被非法降级；
- FieldCell ID 使用 canonical hash；
- transient 数据不能进入 PhotonStore；
- 异常隔离和错误分类一致。

### 11.4 复合 Lens façade

SDK 提供可直接挂载的复合 Lens façade：

```cpp
auto prompt_weaver = OpticalAssemblyLens::create(
    boundary_manifest,
    std::move(internal_lenses),
    assembly_spec);
```

`assembly_spec.inputs/outputs` 显式绑定外部端口与内部 endpoint。façade 隐藏内部 Lens ID、归约 provenance，并由 Nyxia 根据 generation 构造不可变 snapshot。

## 12. C ABI 与 Worker Protocol

### 12.1 干净 ABI 边界

本实现直接切换到 Wavefront ABI，不保留旧入口或扩展表。公开名称不带版本后缀，版本通过结构字段协商：

```c
typedef struct TokmonLensApi {
  uint32_t abi_major;
  uint32_t abi_minor;
  TokmonBytes manifest_cbor;
  void* (*create)(void);
  int32_t (*view)(void*, TokmonBytes optical_input,
                  TokmonOwnedBytes* wavefront_delta,
                  TokmonOwnedBytes* error_frame);
  int32_t (*refract)(void*, TokmonBytes photon_window, TokmonBytes act,
                     TokmonOwnedBytes* result,
                     TokmonOwnedBytes* emitted_drafts,
                     TokmonOwnedBytes* error_frame);
  void (*request_stop)(void*);
  void (*destroy)(void*);
} TokmonLensApi;

TOKMON_LENS_EXPORT TokmonLensApi tokmon_lens_entry(void);
```

### 12.2 Worker 帧

Worker 使用有界 canonical-CBOR IPC：

```text
host → worker: lens.view.request { optical_input }
worker → host: lens.view.result { wavefront_delta | error }
host → worker: lens.refract.request { photon_window, act }
worker → host: lens.refract.result { result, emitted_drafts | error }
```

每个帧绑定：

- ray/beat；
- assembly epoch/hash；
- Lens generation；
- round；
- input/output port；
- schema；
- CellId；
- payload length/hash；
- deadline/cancellation token。

### 12.3 Worker 限制

- Worker 不能请求任意 Band；
- 只能收到已连接输入端口的 delta；
- 只能向 manifest 输出端口发射；
- 不能缓存跨 beat 的 raw handle；
- Worker 退出不会损坏 Photon 历史；
- 超时后输出全部隔离，不进行部分 merge；
- 跨进程 Resonator 禁止；
- 高频大型索引优先进程内可信 Lens、WASM 或宿主共享只读快照。

### 12.4 不提供旧接口适配器

旧接口从未形成已发布实现，本次采用干净切换：C++ 虚表、C ABI、Worker frame、Node.js/Python SDK、官方 Lens 与 manifest 同时更新。加载器只接受 Wavefront ABI；旧符号、旧结构和共享 Surface 写入方式不会进入目标架构。

## 13. Nyxia 职责

### 13.1 Nyxia 是光具座，不是业务总线

Nyxia 负责：

1. artifact、manifest、schema 与签名验证；
2. OpticalAssembly 编译；
3. generation/epoch snapshot；
4. 端口连接与信息流校验；
5. DAG 分层和 Resonator 识别；
6. Wavefront 生命周期和结构共享；
7. 预算、取消、deadline 和异常隔离；
8. 确定性合并与 provenance；
9. Surface Gate、Act arbitration 和 admission；
10. Beam、MountGuard、Worker 和 afterglow；
11. Photon append gate 与崩溃恢复协调。

Nyxia 不负责：

- 理解 AST、RAG、Prompt、模型或业务工作流语义；
- 提供通用 query bus；
- 在引擎代码中写死官方 Lens 组合；
- 自动猜测多个 Provider 的业务优先级；
- 允许运行时 Lens 自行发现和调用其他 Lens；
- 把 Wavefront 当作第二套规范数据库。

### 13.2 “Everything is a Lens”的元层边界

不应把“万物皆透镜”解释为连内存分配器、线程调度器和签名验证器都必须递归实现 `ILens`。否则会产生无限元回归。

工程上的严格定义是：

> 所有会改变业务意义、认知视界、动作候选或组合语义的变换都是 Lens；执行这些变换所需的最小可信调度、存储、隔离和验证机制属于 Nyxia 光具座。

如果未来复用类似 BeatBoard 的数据结构，它只能作为 Nyxia 内部的 Wavefront 存储/索引实现，不得作为 Lens 可见的业务协同 API。

## 14. 因果与副作用边界

### 14.1 同拍 Wavefront 只负责“看和算”

允许：

- Photon 投影；
- AST、符号、索引和 schema 的纯计算；
- 参数化 lookup；
- 上下文筛选；
- 候选排序和 top-k；
- 策略预览；
- 工作流 ready-set 计算；
- Surface fragment 组合；
- Act proposal 生成。

禁止：

- 文件、Git、网络、进程、PTY、模型、MCP/LSP 实际调用；
- Secret 明文读取或消费；
- mount/reconcile；
- 用户审批；
- 发射 committed Photon；
- 修改 Lens 内唯一规范状态；
- 启动脱离 beat lease 的后台任务；
- 等待自身产生的 Act 结果。

### 14.2 跨拍折射负责“做和等”

所有现实动作继续走：

```text
Wavefront act.proposal
→ ActProjectionLens
→ deterministic arbitration
→ act.proposed Photon
→ schema/target/policy/approval admission
→ target Lens.refract
→ external effect or canonical state change
→ result/terminal Photon
→ next Beat SourceProjection
```

### 14.3 Act provenance

Act 应附带最小光程证据：

```text
assembly_epoch/hash
surface_hash
proposal FieldCellId
producer lens/generation
causal Photon ids
critical input CellIds/hashes
aperture/merge rule versions
policy preview hash（仅预览，不替代 admission）
```

不把全部 Wavefront payload 复制进 Photon。大内容使用 Chora ArtifactRef，敏感值只保存受控引用或 hash。

## 15. 谐振安全与收敛

### 15.1 为什么默认不允许任意循环

任意循环可能导致：

- A/B 互相撤销结果；
- 无限生成不同请求 ID；
- 候选集合无界增长；
- 调度顺序改变最终结果；
- Worker IPC 往返爆炸；
- 同拍阻塞现实动作；
- 通过缓存或时钟制造非确定性。

因此普通 Assembly 必须无环。

### 15.2 Resonator 合法性

候选 Resonator 必须同时满足：

1. 所有 Lens `view` 为纯函数；
2. 所有环内 Band 使用有限 join-semilattice；
3. Lens 只追加 delta，不撤销既有 Cell；
4. CellId 对同一语义输出稳定；
5. 输入域有显式最大 Cell/byte/key 数；
6. 最大 round、CPU、内存和 wall-clock 预算固定；
7. 无外部 I/O 或随机源；
8. 不产生 Act proposal，或 Act proposal 只在收敛后的出口 Lens 产生；
9. 非收敛有明确错误/降级语义；
10. dark-lane 包含收敛、极限输入和对抗性测试。

### 15.3 单调性无法完全由宿主证明

对于原生 C++ Lens，Nyxia 无法一般性证明实现函数单调。控制方式包括：

- manifest 明确声明；
- SDK 只暴露 delta emitter，不暴露删除 API；
- schema merge law 强制幂等；
- golden fixed-point 测试；
- 属性测试检查输入扩张不会使输出缩减；
- 优先允许受限 WASM/声明式规则 Lens 进入 Resonator；
- 高信任签名和审查；
- 发现不一致时隔离 generation 并拒绝候选 Assembly。

“声明为 monotone”不是数学证明，因此 Resonator 应保持少量、局部和可替换，不能成为默认编程模式。

### 15.4 分层否定

依赖“某值不存在”的 Lens 不能在同一开放谐振层中执行。它必须位于上游端口 seal 之后：

```text
positive monotone region
→ seal
→ absence/default/negation lens
→ next acyclic layer
```

这避免“当前尚未出现”被误认为“最终不存在”。

## 16. 生命周期、热插拔与空间隔离

### 16.1 拍内固定 Assembly

每拍捕获一次 `OpticalAssemblySnapshot`。当前拍固定：

- Assembly epoch/hash；
- Lens generation；
- 端口连接；
- merge law；
- topology layers；
- Resonator 边界；
- projection endpoints；
- Act routes；
- resource budgets。

mount/unmount/reconcile 只能影响后续新拍。

### 16.2 Assembly lease

当前拍为所使用的 Lens、Worker、DerivedFieldHandle 和共享内存持有统一 lease：

- 已开始的拍可以按 drain 策略完成；
- 新拍不再进入旧 generation；
- 旧 generation 不能在 publish 后获取新 Beam；
- 超过 afterglow deadline 的 Worker 被终止；
- 已产生但未合并的旧拍 Wavefront 被丢弃；
- 已提交 Photon 永远保留。

### 16.3 拔出复合 Lens

拔出 `OpticalAssembly` 后：

- 其内部所有直接 Wavefront contribution 在新拍归零；
- 其对外端口不再连接；
- optional 下游进入 DefaultLens；
- required 下游使候选 Assembly 发布失败；
- 内部缓存、handle、Worker 和连接由 MountGuard 回收；
- 历史 Photon 与已经发生的现实效果不被删除；
- 若历史 Photon 被其他 Lens 重新解释，必须标明来源，不能宣称历史认知痕迹物理消失。

### 16.4 局部空间边界

子 Assembly 只暴露声明的边界端口。内部 Band 即使名称相同，也不能被外部自动读取：

```text
AgentSpace
  internal: syntax.index, rag.raw-candidate
  exposed: model.context, act.proposal
```

这使空间封装不再只依赖命名约定。

## 17. 安全与信息流

### 17.1 端口级标签

每个 Band/Cell 至少携带：

- trust；
- sensitivity；
- allowed audience；
- origin generation；
- causal source；
- redaction policy；
- exportability。

连接编译必须拒绝：

- 高敏感 Band 流入低信任 Worker；
- Secret 明文进入 Model/UI/CLI/Diagnostic Band；
- 未经降敏 Lens 直接降低 sensitivity；
- 未签名 Provider 输出进入高信任 exclusive port；
- transient handle 跨越不支持的进程或拍边界。

### 17.2 Secret

Wavefront 默认也不能携带 Secret 明文。需要现实调用时：

```text
SecretRef FieldCell
→ Act proposal
→ admission
→ Cista/MountGuard short-lived binding
→ target refract
```

明文只在受控 Beam 内短时出现，不进入普通 FieldCell、Photon、Surface、trace、cache 或 Worker IPC。

### 17.3 资源预算

预算层级：

```text
daemon
  → ray
    → beat
      → assembly
        → lens
          → port/band
            → cell
```

至少强制：

- 最大 Lens 执行次数；
- 最大传播层数；
- 最大 Resonator round；
- 最大 Cell 数和字节；
- 最大单 Cell 大小；
- CPU/wall-clock；
- Worker IPC 帧数；
- DerivedFieldHandle 数量和内存；
- 每 Band top-k 或 cardinality；
- diagnostic/trace 采样上限。

## 18. 错误语义

### 18.1 编译期错误

| 错误 | 含义 |
|---|---|
| `port_unconnected` | required 输入没有来源 |
| `schema_mismatch` | Band 相同但 schema 不兼容 |
| `cardinality_mismatch` | one/many 与连接或 merge 不兼容 |
| `ambiguous_connection` | 自动接线出现多个合法来源 |
| `merge_law_missing` | 多源输出没有显式合并规则 |
| `trust_flow_denied` | 信息流越过不允许的信任边界 |
| `unguarded_cycle` | 环中没有 Delay，也未声明合法 Resonator |
| `non_lattice_resonator_band` | 谐振 Band 不满足允许的 join 约束 |
| `budget_unsatisfiable` | 静态预算下界已超过上限 |
| `act_route_ambiguous` | Act 没有唯一现实折射目标 |

这些错误使候选 Assembly 无法发布，不影响当前 active epoch。

### 18.2 传播期错误

| 错误 | 处理 |
|---|---|
| `lens_view_failed` | 隔离该输出；critical Lens 使本拍失败 |
| `output_schema_invalid` | 拒绝 Cell，记录 provider generation |
| `optical_budget_exceeded` | 终止相关子空间或整拍，不采用不完整结果 |
| `resonator_not_converged` | 按声明 fail/degrade，记录最后稳定 hash/delta 统计 |
| `stale_generation` | 当前 snapshot 外的输出拒绝合并 |
| `worker_protocol_violation` | 终止 Worker，隔离 generation |
| `provenance_invalid` | 拒绝引用不可见输入的输出 |
| `sensitivity_violation` | fail closed，并产生有界安全诊断 |

### 18.3 降级

降级必须由明确 Lens/连接表达，而不是异常处理中的临时分支。例如：

```text
PrimaryProvider ─ success ─→ output
                └ failure ─→ FallbackLens → degraded.output
```

降级结果必须携带：

- 缺失/失败来源；
- fallback Lens generation；
- 是否改变质量、安全或成本；
- 是否允许继续产生 Act。

## 19. 可观测性与回放

### 19.1 Optical Trace

默认把以下信息写入有界 telemetry，而不是 Photon：

```text
ray / beat / assembly epoch
lens / generation / port
input CellId/hash/count/bytes
output CellId/hash/count/bytes
topology layer / resonator round
cache hit
duration / CPU / allocation
merge/aperture decision
status / error
```

敏感 payload 不进入 trace。

### 19.2 光程 provenance

每个最终 Surface fragment 和 Act proposal 都应能展开为有界 provenance DAG：

```text
Photon ids
→ Source FieldCells
→ intermediate Lens generations
→ merge/aperture decisions
→ projection output
```

UI/doctor 可以展示摘要，完整大图按需生成 artifact。

### 19.3 回放级别

| 级别 | 内容 | 外部副作用 |
|---|---|---|
| R0 | Photon transcript | 否 |
| R1 | 指定 Assembly 重建 Wavefront/Surface | 否 |
| R2 | 重放纯 Lens 与 Resonator，验证 hash | 否 |
| R3 | 允许受控现实折射 | 是，必须显式授权 |

R1/R2 回放不访问真实文件、网络、模型或 Secret。相关输入必须来自已提交 Photon、固定 artifact 或合法 snapshot reference。

## 20. 性能模型

### 20.1 与两阶段 SOQ 候选设计的成本比较

按对照草案实现时，SOQ 适合少量点查询，主要成本近似：

```text
O(N derive + Q query)
```

波前 DAG 主要成本近似：

```text
O(V lens execution + E delta routing + M merge)
```

Resonator 为：

```text
O(R × (V_scc + E_scc + M_scc))
```

其中 R 必须很小且有硬上限。

### 20.2 优化策略

1. 只向连接端口路由，不建立全局可见 Field map；
2. immutable Cell/large handle 结构共享；
3. 同层并发、稳定 merge；
4. 基于输入 Cell hash 的 memoization；
5. `on_seal` Lens 避免重复全量计算；
6. `on_delta` Lens 使用 semi-naive 增量传播；
7. 同一请求 CellId 自动去重；
8. 大 payload 使用 DerivedFieldHandle 或 ArtifactRef；
9. 无输入的 Source Lens 走单次执行快速路径；
10. 编译后的端口索引使用整数 ID，运行时不做字符串查找；
11. Resonator 默认限制在进程内小型纯计算子图；
12. ProjectionLens 只读取最终所需 Band，不遍历全部中间态。

### 20.3 必测基线

- 20 个空 Source Lens 的单拍开销；
- 线性 20 层串联；
- 4/16/64 分支并行与合束；
- 1K/10K/100K FieldCell 的路由和 merge；
- 2/4/8 round Resonator；
- in-process/WASM/Worker delta 往返；
- 大型 AST handle 零拷贝与 CBOR fallback；
- cache hit/miss；
- generation reconcile 与在途 Beat lease；
- 非收敛和预算失败的尾延迟；
- 百万 Photon 历史下 SourceProjection 和 Surface Gate 稳定性。

## 21. 与未实现 BeatBoard/SOQ 草案的关系

### 21.1 两份设计是候选方案，不是现状与迁移目标

BeatBoard/SOQ 草案尚未落地，因此本文不定义从既有 SOQ runtime 迁移、兼容旧 SOQ provider 或逐步下线公共 query API 的工程任务。两份文档当前应被理解为解决同一问题的不同候选架构：

```text
候选 A：Derive → Freeze → SOQ/Coordinate
候选 B：Typed Wavefront → OpticalAssembly propagation
```

两者可以共享部分底层数据结构，但不应在尚未决策和实现前同时固化两套公共协同 API。

如果选择本文方案，Nyxia 内部可以使用类似 BeatBoard 的数据结构保存：

- 已产出 Cell；
- 端口索引；
- DerivedFieldHandle；
- cache；
- generation lease；
- trace。

区别在于公共语义：

```text
不推荐：Lens 主动 query 全局 Board 上的 Provider
推荐：Nyxia 按已编译连接把入射 delta 送到下游 Lens
```

因此可以复用的是冻结索引、generation lease 和缓存等实现思想，而不是先实现 SOQ 再包一层 Wavefront API。

### 21.2 不预设 SOQ 兼容桥

由于不存在已部署的 SOQ provider，本文不设计 `SoqBridgeLens`，也不把“兼容旧 SOQ”列为交付要求。推荐直接实现：

```text
request Band
→ typed Query Lens
→ response Band
```

只有在未来先独立落地 SOQ、并已经产生必须兼容的第三方 provider 后，才应通过单独 ADR 评估临时 Bridge。该 Bridge 不属于本文目标架构，且不得成为新 Lens 的推荐入口。

### 21.3 选择建议

| 场景 | 建议 |
|---|---|
| 近期只需以最小改动接通 Janus 与几个现有 Lens | 可以选择实现 BeatBoard `get()` 草案，但这是替代路线而非现状迁移 |
| 需要大量稳定参数化能力 | Query Lens + typed ports |
| 需要复用完整多 Lens 子系统 | OpticalAssembly |
| 需要同拍纯循环 | 小型 Resonator |
| 需要外部 I/O/等待/审批 | Act/Photon + CausalDelay |
| 第三方 Worker 高频点查询 | 直接使用 typed-port delta protocol，按预算评估 IPC |

## 22. 与现有 Lens Law 的演进

### 22.1 保留的规则

以下规则保持不变：

1. **Append Only**：只有事实提交 gate 能产生 committed Photon；
2. **Pure View**：相同输入与版本产生相同派生；
3. **Act Before Reality**：现实动作先有结构化意图与准入；
4. **No Side Channel**：Lens 之间不得对象调用或隐藏通信；
5. **Patterned Refraction**：只有声明且校验通过的 Act 能进入 `refract`；
6. **Darkness Means Stop**：没有新 Act、输入或在途 Beam 时自然停机；
7. **Zero New Contribution**：拔镜后新拍无该 generation 的直接贡献。

### 22.2 需要替换的规则

现有：

```text
Linear Fold：所有显像贡献按 LightPath 顺序折叠
```

建议替换为：

```text
Composed Propagation：
所有拍内派生必须沿不可变 OpticalAssembly 的显式端口连接传播；
无环区域按确定拓扑执行，反馈只允许通过 CausalDelay 或合法 Resonator；
任意复合结果仍具有 Lens 边界。
```

### 22.3 LightPath 的正式定义

LightPath 不再只是一组隐式线性 fold，而是：

```text
mounted Lens generations
+ OpticalAssemblySpec
→ compiled OpticalAssemblySnapshot
```

只有显式连接或唯一自动接线才传递中间 Band；无输入 Lens 表现为从 PhotonWindow 派生 Field 的 Source Lens。

顶层 daemon 仍只激活一个不可变 LightPath/Assembly epoch；复杂空间被封装在该快照内部，不允许 Lens 在运行时私建第二条光路。

## 23. 一次性实现状态

### 23.1 Wavefront 基础设施（已完成）

- 定义 `BandId`、Port、FieldCell、Wavefront、provenance；
- 定义 canonical CellId；
- 实现端口 schema registry 和 merge law registry；
- 把 Surface contribution 定义为终端 Band 投影；
- 官方 Lens 全部切换到唯一 Wavefront 接口；
- 不以尚未实现的 BeatBoard/SOQ 公共接口作为本阶段前置条件。

验证结果：

- golden ray 的 Surface 语义稳定；
- 删除 Wavefront/cache 后可重建；
- 所有 Cell 有 generation/input provenance；
- transient Band 无法进入 PhotonStore。

### 23.2 无环 OpticalAssembly（已完成）

- 增加 Lens typed ports；
- 实现 serial/parallel/split/prism/merge/aperture；
- 实现 Assembly 编译、拓扑层和不可变 snapshot；
- 实现结构 Lens 与复合 façade；
- 扩展 dark lane 验证连接、预算和信息流。

退出条件：

- 后游 Lens 能消费上游同拍输出；
- 同层并发与串行执行得到相同 hash；
- 复合 Assembly 可以作为单 Lens 嵌套；
- 未连接 required port 阻止发布。

### 23.3 官方 Lens 与复合镜组（已完成）

- 把二十官方 Lens 的显像输出全部声明为 Band/port；
- 实现 Identity/Splitter/Prism/Merge/Aperture/Projection 结构 Lens；
- 实现可嵌套的 `OpticalAssemblyLens` façade；
- 允许 Prompt、查询、检索和策略子系统用相同结构构造复合镜组；
- LightPath 配置可以物化连接、Resonator 与预算。

实现验证：

- 上游同拍 Field 能经 merge/aperture/projection 到达 Surface；
- 复合 Assembly 边界隐藏内部端口并归约 provenance；
- optional/required 由端口和编译结果明确表达；
- Surface/Act 均保留 Field 与 Assembly 证据。

### 23.4 多语言 Wavefront Protocol（已完成）

- Worker Protocol 增加 input/output delta；
- C ABI 直接使用 Wavefront 输入与 delta 输出；
- 为 C++、Node.js、Python 提供等价 typed-port SDK，并允许受限 WASM runtime kind；
- 保证运行时不引入另一套 provider/query 公共接口。

实现验证：

- C ABI、Node.js 与 Python contract tests 通过；
- Worker 只能访问已连接端口；
- typed Query Lens 在不同运行时产生相同 request/response hash；
- 新官方 Lens 只通过 typed ports 暴露拍内能力。

### 23.5 因果延迟（已完成）

- 实现 `CausalDelayLens`；
- 建立 Band → Act proposal → Photon schema 映射；
- 强制所有非纯反馈跨拍；
- 完善恢复、幂等、outcome-unknown 和 cancellation。

退出条件：

- 未保护循环在 publish 前失败；
- 外部 I/O 无法从 `view`/Wavefront 路径触发；
- 延迟反馈具有完整 Act/Photon 关联；
- Lens 不会同步等待自身 Act 结果。

### 23.6 有限谐振（已完成）

- 实现 SCC 分析和 Resonator 编译；
- 限定首批 merge law；
- 实现基于 IncidentWave canonical hash 的 delta 去重传播；
- 增加收敛 hash、round trace 和非收敛错误；
- 优先支持 WASM/声明式纯 Lens；
- 增加真实结构与对抗性单元测试。

退出条件：

- 收敛结果与调度顺序无关；
- 极限输入在预算内成功或明确失败；
- 非单调/非 lattice Band 无法进入 Resonator；
- Resonator 不执行 Act、I/O 或 Secret 消费。

### 23.7 规范收敛（已完成）

- 选择本文作为正式架构；
- 将未实现 BeatBoard/SOQ 文档保留为被取代的设计对照；
- 保留可复用的冻结索引、generation lease 和 cache 思想；
- 更新 `DESIGN.md`、SDK、示例和术语；
- 确保正式规范只暴露一套拍内协同模型。

## 24. 测试与验收

### 24.1 组合定律测试

- `L ∘ I == L`；
- `I ∘ L == L`；
- 合法串联满足结合律；
- tensor 分支调度顺序不改变输出；
- `set_union/top_k/custom_lattice` 满足声明代数；
- Assembly 封装前后边界输出相同；
- 内部端口不因同名被外部读取。

### 24.2 确定性测试

- 相同 Photon prefix、Assembly hash 和 artifact 产生相同 Wavefront/Surface hash；
- 并发完成顺序随机化不改变结果；
- canonical CBOR map 字段顺序不改变 CellId；
- cache 命中与禁用 cache 结果相同；
- Worker 与 in-process 等价 Lens 结果相同；
- reconcile 并发时当前拍不混用 generation；
- 回放可重建最终 Act proposal provenance。

### 24.3 安全测试

- Lens 无 registry/query API；
- 未声明端口无法读取/写入；
- 高敏感 Band 不能流入低信任 Worker；
- Secret 明文不进入 FieldCell/trace/Photon/Surface；
- view 无法取得 filesystem/network/process/model handle；
- Worker 超时或协议违规后输出不被部分接纳；
- stale generation delta 被拒绝。

### 24.4 循环测试

- 普通 DAG 出现环时 publish 失败；
- Delay 环可跨拍推进并恢复；
- 合法 Resonator 在预期 round 收敛；
- 输出不断产生新 ID 时触发预算/非收敛错误；
- 调度顺序扰动不改变固定点；
- absence/negation Lens 只能在 sealed stratum 后运行；
- Resonator 尝试产生 Act 时被拒绝。

### 24.5 生命周期测试

- 拔出原子 Lens 后新拍无其直接 Cell；
- 拔出复合 Assembly 后内部所有端口失效；
- 当前拍持有 lease 时旧 generation 可以 drain；
- 新拍不能路由到旧 generation；
- Worker kill 后缓存和 Photon 历史一致；
- optional 分支进入显式 DefaultLens；
- required 分支缺失时候选 Assembly 不发布。

### 24.6 端到端验收

- Textus + Enso + Techor + RheaCatalog + Janus 完成同拍组合；
- Syntax + Completion 无 SOQ 完成位置查询；
- 多检索器合束、top-k 和 token aperture 稳定；
- model/file/process/MCP 操作只能通过 Act/Photon；
- 百万历史 Photon 下 Surface 仍有界；
- 删除全部 projection/field cache 后输出一致；
- dark lane 能比较旧/新 Assembly 的 Surface、Act、成本和光程差异。

## 25. 风险与权衡

### 25.1 组合模型明显更复杂

从平坦数组演进为可嵌套镜组，需要端口 schema、连接编译、传播计划、合并代数和更强开发工具。其收益是得到真正的局部组合和复合闭合；如果项目短期只有少数点查询，选择实现对照草案中的 SOQ 可能具有更好的投入产出比，但那是另一条候选实现路线。

### 25.2 Wavefront 可能退化为另一块全局黑板

如果 SDK 提供按 Band 全局搜索、任意读取或动态 provider lookup，设计会重新退化为 BeatBoard。必须坚持 Lens 只收到已连接 `IncidentWave`。

### 25.3 端口数量可能爆炸

过细端口造成配置繁琐，过粗端口形成万能对象。端口应围绕稳定语义和复用边界，而不是为每个私有函数建立一个 Band。

### 25.4 固定点容易被滥用

开发者可能用 Resonator 掩盖错误的循环职责。默认应拒绝循环，先推荐：

1. 抽出共同基础 Lens；
2. 明确 Coordinator Lens；
3. 改成 DAG 多层传播；
4. 使用因果延迟；
5. 最后才考虑 Resonator。

### 25.5 “纯”不等于廉价

AST 遍历、向量检索和 schema 编译即使无副作用，也可能消耗大量 CPU/内存。所有 Lens 和端口仍需硬预算、缓存和异步重建路径。

### 25.6 经典 Lens 数学与工程 Lens 不完全相同

Tokmon 的 `refract` 包含受控现实动作，不等同于经典函数式 `Lens<S,A>` 的纯 `put`。本文使用“Lens”作为统一工程光学构件，并采用组合、局部性和守恒约束；不应声称所有外部副作用天然满足经典 GetPut/PutGet/PutPut 定律。

需要形式化时，更准确的模型是：

- 拍内 `view`：typed optical/dataflow transducer；
- 组合：对称幺半/可追踪光学范畴；
- 跨拍 `refract`：受 Act/Photon 约束的因果效果边界。

### 25.7 顶层直线语义需要重新解释

如果 `DESIGN.md` 的“只有一条直线光路”被理解为禁止任何内部并行、分束和合束，则本文无法兼容。建议将它收敛为：

> 每个 Ray 在任一 epoch 只由一个不可变 OpticalAssemblySnapshot 解释；Assembly 外不存在 Lens 私建光路或横向通信。

这样既保留单一权威光路，又允许镜组内部具有封装的空间结构。

## 26. 被拒绝的替代方案

### 26.1 共享可变 Surface

让后执行 Lens 直接读取/修改前一个 Lens 的 Surface，虽然改动最小，但会导致：

- 执行顺序成为隐藏依赖；
- 并行困难；
- provenance 和冲突模糊；
- Lens 可以修改他人贡献；
- 无法表达局部空间和明确端口。

### 26.2 任意 Lens RPC

对象引用、service locator 或通用 RPC 会重新引入调用环、锁、悬空 generation、跨语言语义差异和副作用绕行，因此拒绝。

### 26.3 所有协同都 Photon 化

把 AST 查询、候选集和机械中间状态全部提交为 Photon，可以保持因果简单，但会导致事实密度失控、存储膨胀和多拍延迟。Photon 只保存最小充分规范事实。

### 26.4 全局无界固定点

把所有 Lens 放入一个全局 Datalog/actor 式迭代器，会使成本、终止和错误隔离不可控。谐振必须局部、显式、有限且默认关闭。

### 26.5 用真实光学参数驱动业务

使用角度、焦距、折射率等数值模拟路由会把类型和策略问题转成难以验证的浮点隐喻。本文只保留对组合、安全和执行有明确价值的光学结构。

## 27. 规范性规则汇总

以下规则是 SDK 与运行时的正式约束：

1. Lens 不得持有、查找或调用另一个 Lens；
2. Lens 的同拍输入只能来自 PhotonWindow 和已连接的 IncidentWave 端口；
3. Lens 只能向 manifest 声明的输出端口发射不可变 FieldCell；
4. Wavefront 是 transient 派生场，不是 Photon、事实或恢复依据；
5. 所有多源端口必须声明确定 MergeLaw；
6. 串联、并行、分束、选择、合束、光阑、延迟和谐振必须由结构 Lens 表达；
7. 任意合法复合镜组必须可以封装成具有相同外部形状的 Lens；
8. 普通 Assembly 必须无环；
9. 每个反馈环必须包含 CausalDelay 或合法 Resonator；
10. Resonator 内 Lens 必须纯、单调、有限、无 Act、无 I/O；
11. Resonator 必须有 max round、deadline、execution、Cell 和 byte 预算；
12. 不收敛必须明确失败或走声明式降级，不能静默采用最后一轮；
13. absence/negation 只能在上游 sealed 后执行；
14. 外部 I/O、模型、文件、进程、Secret、审批和规范状态变化必须走 Act/refract；
15. Lens 不得在 view 中等待 Act 结果；
16. 所有 Act 必须绑定 Assembly epoch、目标 generation 和关键光程 provenance；
17. Nyxia 可以内部使用类似 BeatBoard 的冻结存储结构，但不得向 Lens 暴露通用 query/provider API；
18. 所有连接、Provider 选择、merge 和 fallback 必须在 Assembly 发布前确定；
19. 当前拍只能使用一次捕获的不可变 AssemblySnapshot；
20. 拔镜后新拍必须没有该 generation 的直接 Field/Surface/Act contribution；
21. 历史 Photon 不因拔镜、缓存删除或镜组重编译而改变；
22. 端口 trust/sensitivity 流必须由宿主强制；
23. Secret 明文不得进入 Wavefront、Photon、Surface、trace 或普通 Worker IPC；
24. 所有运行时形态必须共享 Band/Port/Cell/Assembly/Act/Photon 语义；
25. 删除所有 Wavefront/projection cache 后必须能够重建相同规范输出。

## 28. 已采用决策

`tokmon-n` 已采用本文模型，不实现 BeatBoard/SOQ 公共协同 API，并坚持三项架构决策：

1. **Capability 是端口类型，不是可调用服务**；
2. **协同是波前传播，不是 Lens RPC**；
3. **复合镜组仍然是一片 Lens**。

当前最终光路为：

```text
Committed PhotonWindow
        ↓ SourceProjectionLens
Typed immutable Wavefront
        ↓
serial / parallel / prism / merge / aperture
        ↓
optional bounded Resonator
        ↓
ProjectionLens → Surface + Act proposals
        ↓
Act arbitration / admission
        ↓
target Lens.refract
        ↓
new committed Photon
        ↓
next Beat
```

其核心边界可以概括为：

> **同拍内，光在镜组中传播并形成当前认知；跨拍时，折射以 Photon 改变系统与世界。**

在这一模型中，类似 BeatBoard 的内部结构可以存在，但只是光场显存；Query 可以存在，但只是一片 request/index/answer Lens；并行、路由、合并和反馈也不再是 Nyxia 的特殊业务 API，而是能够继续组合、封装、替换和拔出的光学构件。
