# `tokmon-n` 多透镜有限同步与因果异步协同设计

> 文档类型：架构设计草案 / SDK 与运行时演进规范  
> 审查基线：`tokmon-n@0fa9a0b92454f2a122239cf2b50e0878c9675d79`  
> 编写日期：2026-08-26  
> 适用范围：Nyxia、C++ SDK、C ABI、多语言 Worker、正式 Lens、Calculator 示例 Lens、CLI/Desktop  
> 上位约束：[DESIGN.md](DESIGN.md)  
> 配套路线图：[tokmon-n-rcld-improvement-roadmap.zh.md](tokmon-n-rcld-improvement-roadmap.zh.md)

## 1. 执行摘要

`tokmon-n` 当前采用直线 LightPath：Nyxia 读取同一份 `PhotonWindow`，按固定顺序调用每个 Lens 的 `view()`，合并 Surface contribution 和 Act proposal，再选择一个 Act 路由到唯一目标 Lens 的 `refract()`。该设计降低了循环调用、重入和死锁风险，但也意味着同一拍内后一个 Lens 看不到前一个 Lens 刚刚派生的 Surface，更不能同步查询其他 Lens 的内部派生状态。

本设计在不把 LightPath 改造成任意网状调用图的前提下，引入两种互补协同机制：

1. **有限同步协同**：在同一拍内，所有 Lens 先从同一 Photon 前缀派生并冻结只读状态；随后 Lens 可以通过 Nyxia 托管的能力查询接口，用结构化参数同步查询其他 Lens 的冻结状态。查询可以执行复杂计算，但必须只读、确定、受预算约束、无外部副作用、无递归跨 Lens 查询；
2. **因果异步协同**：凡是会改变状态、访问外部世界、等待不确定时间、需要审批或可能重试的操作，都继续通过 `Act → admission → target Lens.refract() → Photon → 下一拍` 完成。

一句话概括：

> 同拍同步协同只负责“读和算”，跨拍异步协同负责“做和等”。

推荐在 Nyxia 中增加一个拍内临时协同层，本文暂称 **BeatBoard（同拍折光板）**，并增加一个宿主中介的 **Synchronous Optical Query（SOQ，同步光学查询）** 接口。Lens 从使用体验上可以同步带参查询另一个能力提供者，但不持有另一个 Lens 的对象、函数指针、线程、锁或生命周期。

## 2. 问题背景

### 2.1 当前直线光路的实际语义

当前 `RayTracingEngine::view()` 的核心过程是：

```text
PhotonStore.read_ray(ray)
        ↓
PhotonWindow
        ↓
按 LightPath 顺序循环
        ↓
为每个 Lens 创建独立 SurfaceBuilder
        ↓
Lens.view(同一 PhotonWindow, 独立 builder)
        ↓
合并所有 contribution 和 proposal
```

因此当前顺序是确定的，但 Lens 之间的 `view()` 在语义上彼此隔离：

- 每个 Lens 看到同一个已提交 Photon 前缀；
- 后执行的 Lens 看不到前一个 Lens 本拍刚产生的 Surface；
- Surface 是最终汇总结果，不是逐镜传递的中间寄存器；
- Lens 之间不存在正式的对象级同步调用接口；
- 业务接力主要依赖 Act 执行后产生的新 Photon，在下一拍继续。

这使系统具备较清晰的因果边界，但产生了三个现实问题：

1. Textus、Enso、Techor 等 Lens 即使分别发布 `model.messages`、`model.context`、`model.tools`，Janus 也无法在同一拍直接组合这些结果；
2. 代码分析、代码高亮、补全、诊断等紧密功能需要重复解析同一文件，或不得不把机械中间状态写成 Photon；
3. 为复用能力而把多个 Lens 合并成一个大 Lens，会削弱可替换、可测试和可拆卸性。

### 2.2 不能直接退回任意对象调用

最直接但不合适的做法是让 Lens 持有其他 Lens 的 C++ 接口：

```cpp
auto ast = syntax_lens_->parse(file);
auto completions = completion_lens_->complete(ast, cursor);
```

这种做法会使 LightPath 表面保持直线，内部却形成隐藏网状调用图，并重新引入：

- A 调 B、B 调 A 的递归环；
- 锁顺序不一致和死锁；
- mount epoch 切换时的悬空指针；
- in-process、C ABI、Worker 和远程 Lens 之间不同的调用语义；
- 调用目标与具体实现绑定，替换提供者需要重新编译调用方；
- 调用过程绕开 Photon、Act、admission、审计和回放；
- Lens 在同步栈中执行文件、网络或进程 I/O，使一拍无限阻塞。

因此本设计不提供通用 `Lens::call(other_lens)`，而提供受 Nyxia 约束的“能力查询”。

## 3. 设计目标与非目标

### 3.1 目标

本设计必须做到：

1. 保留一条由 Nyxia 托管、顺序确定的 LightPath；
2. 允许 Lens 在同一拍进行带复杂参数的同步只读查询；
3. 允许多个 Lens 查询同一提供者，也允许 A、B 分别查询对方的冻结基础状态；
4. 不要求将 AST、候选集、临时索引、工具目录等机械派生状态写入 PhotonStore；
5. 不允许同步查询改变外部世界或规范状态；
6. 能力调用不绑定具体 Lens ID，允许提供者替换、卸载和跨进程实现；
7. 查询输入、输出、提供者、generation、耗时和结果哈希可观测；
8. 相同 Photon 前缀、LightPath snapshot、查询请求和实现版本产生确定结果；
9. 老 Lens 在不实现新接口时仍可继续挂载运行；
10. 与现有单 Act 提交和跨拍 Photon 因果循环兼容。

### 3.2 非目标

本设计不试图：

- 支持任意 Lens 对任意 Lens 的通用 RPC；
- 在同步查询中写文件、调用模型、访问网络、启动进程或消费 Secret；
- 允许查询处理器递归查询第三个 Lens；
- 自动求解任意循环依赖；
- 将所有 Photon 协同替换为内存调用；
- 为跨进程查询承诺与进程内函数调用相同的延迟；
- 用同步查询替代 Act admission、审批、幂等和审计。

## 4. 核心术语

| 术语 | 定义 |
|---|---|
| Beat | Nyxia 对一个 Ray 执行一次观察、协同、选择和折射的逻辑拍 |
| Derive | Lens 根据冻结 Photon 前缀准备拍内只读派生状态的阶段 |
| BeatBoard | 当前拍的临时只读派生状态与能力注册表 |
| Frozen Lens State | 某个 Lens 在 Derive 阶段生成、冻结后不可修改的拍内状态 |
| Capability | 与具体 Lens ID 解耦的查询能力名，例如 `syntax.at-position` |
| SOQ | 由 Nyxia 中介的同步带参能力查询 |
| Coordinate | Lens 读取 BeatBoard、发起 SOQ、形成最终 Surface 和 Act proposal 的阶段 |
| Act | 可能产生规范状态或外部副作用的结构化意图 |
| Photon | 已提交、可审计、可恢复、可跨拍观察的因果事实 |
| Query Provider | 声明并实现某项只读查询能力的 Lens |
| Query Consumer | 在 Coordinate 阶段请求该能力的 Lens |

## 5. 总体架构

### 5.1 两阶段同拍执行

每一拍由两个只读阶段和一个副作用阶段组成：

```text
┌──────────────────────────────────────────────────────────┐
│ PhotonWindow：本拍统一、冻结的已提交事实前缀             │
└──────────────────────────────┬───────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────┐
│ 阶段一：Derive                                            │
│ 按 LightPath 固定顺序调用所有 Lens                        │
│ 发布 Surface contribution、Frozen State、Query Provider  │
└──────────────────────────────┬───────────────────────────┘
                               │ Barrier
                               ▼
┌──────────────────────────────────────────────────────────┐
│ BeatBoard.freeze()                                       │
│ 状态、provider、generation、schema、路径顺序全部冻结      │
└──────────────────────────────┬───────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────┐
│ 阶段二：Coordinate                                        │
│ 按同一 LightPath 固定顺序执行                             │
│ get() 读取现成状态；query() 进行同步带参纯查询            │
│ 生成最终 Surface contribution 和 Act proposal            │
└──────────────────────────────┬───────────────────────────┘
                               │
                               ▼
┌──────────────────────────────────────────────────────────┐
│ Act arbitration / admission / 单目标 refract             │
└──────────────────────────────┬───────────────────────────┘
                               │
                               ▼
                    新 Photon，进入下一拍
```

### 5.2 为什么需要冻结屏障

如果没有屏障，查询结果会取决于调用发生时哪个 Lens 已经运行：

```text
A 已更新、B 未更新 → C 读到半新半旧状态
```

冻结屏障保证 Coordinate 阶段所有 Lens 看到同一版本：

```text
BeatBoardKey = ray + beat + epoch + path_hash + input_prefix_hash
```

同一拍内：

- Provider 集合不变化；
- Lens generation 不变化；
- Frozen State 不变化；
- 已缓存的相同查询结果不变化；
- mount/unmount 只能影响后续拍。

### 5.3 顺序不是乱序

第一阶段和第二阶段都默认沿当前 LightPath 串行执行。第一阶段中的 Lens 不读取其他 Lens 本阶段输出，因此在语义上彼此独立；但实现顺序仍然确定。未来即使并行执行 Derive，也必须按照 LightPath index 确定性合并结果。

Coordinate 阶段同样按 LightPath 顺序运行。同步查询在调用点立即返回；Provider 的纯查询函数使用已经冻结的状态，因此不依赖 Provider 在 Coordinate 阶段是否已经执行。

## 6. 有限同步协同

### 6.1 两种同步读取

#### 6.1.1 `get()`：读取预先派生的值

适用于已经在 Derive 阶段形成的完整结果：

```cpp
auto messages = optical.get("model.messages", "active-ray");
auto contexts = optical.get_all("model.context");
auto tools = optical.get_all("model.tools");
```

`get()` 本身不执行 Provider 代码，只在冻结的 BeatBoard 索引中读取数据。

#### 6.1.2 `query()`：带参数同步纯查询

适用于结果空间很大、无法预先枚举，或者查询结果取决于调用方参数的场景：

```cpp
auto result = optical.query("syntax.at-position", cbor::object({
    {"file", "src/main.cpp"},
    {"line", 120},
    {"column", 16},
    {"include_inherited", true}
}));
```

执行过程不是从共享板中寻找一个预先存在的“第 120 行第 16 列结果”，而是：

```text
Syntax Lens 在 Derive 阶段构建并冻结 AST/符号索引
        ↓
Completion Lens 在 Coordinate 阶段提交位置参数
        ↓
Nyxia 验证 capability、schema、预算和 provider generation
        ↓
调用 Syntax Lens 的纯 query handler
        ↓
handler 使用 Frozen Syntax State + request 现场计算
        ↓
同步返回结构化结果
```

因此有限同步不是“只能空参查询”。请求可以是任意满足已声明 Schema 和大小预算的 CBOR 结构，查询算法也可以很复杂。

### 6.2 建议接口

```cpp
struct OpticalQueryRequest {
  std::string capability;
  cbor::Value parameters;
  std::chrono::milliseconds timeout;
  std::size_t max_response_bytes;
};

class OpticalContext {
 public:
  Result<cbor::Value> get(
      std::string_view channel,
      std::string_view key) const;

  Result<std::vector<cbor::Value>> get_all(
      std::string_view channel) const;

  Result<cbor::Value> query(
      OpticalQueryRequest request) const;
};
```

Provider 接口应刻意保持受限：

```cpp
class IOpticalQueryProvider {
 public:
  virtual Result<cbor::Value> query(
      const FrozenLensState& state,
      std::string_view capability,
      const cbor::Value& parameters,
      const QueryBudget& budget) const = 0;
};
```

Provider 的 `query()` 不接收 `OpticalContext`、`SurfaceBuilder`、`RefractionBeam` 或可写 `PhotonStore`，从类型层面减少递归跨 Lens 查询和副作用出口。

### 6.3 可以有多复杂

有限同步允许：

- 用文件、位置、语言、约束和选项查询 AST/符号；
- 在冻结索引上执行全文搜索、向量检索或图遍历；
- 根据 token budget 选择消息和上下文片段；
- 编译和验证工具 JSON Schema；
- 计算代码高亮、补全候选、类型推导和诊断关联；
- 对候选 Act 做纯策略预览；
- 对工作流快照进行可运行节点计算；
- 对已冻结模型目录进行能力匹配。

复杂度受到预算限制，而不是受到“只能空参”限制。

### 6.4 不允许做什么

同步 query handler 不得：

- 修改 Provider 的 Frozen State；
- 修改调用方或其他 Lens 状态；
- 发射 Photon；
- 提出或执行 Act；
- 访问文件系统、网络、进程、模型、系统 keyring；
- 等待用户审批；
- 获取可长期保存的另一个 Lens 指针；
- 再调用 `OpticalContext::query()`；
- 启动脱离当前 query budget 的后台任务；
- 让结果依赖未冻结的时钟、随机数或全局可变状态。

### 6.5 为什么禁止嵌套查询

如果允许 Provider 在处理 A 的查询时继续调用 B，而 B 又查询 A，就会产生隐藏调用环：

```text
Completion.query
  → Syntax.query
      → TypeInference.query
          → Completion.query
```

本设计允许调用方查询任意 Provider，但 Provider 的 query handler 本身不能再跨 Lens 查询。这样同步调用图的最大深度固定为一层：

```text
Consumer → Nyxia → Provider
```

Provider 如果需要其他能力，应在 Derive 阶段从 Photon/Frozen input 独立准备，或者把真正的多阶段依赖建模为明确阶段或跨拍 Act/Photon。

### 6.6 “互相查询”的准确边界

A 和 B 可以在同一拍分别查询对方的基础状态：

```text
Derive：A_state = f(PhotonWindow)
Derive：B_state = g(PhotonWindow)
Freeze
Coordinate：A 查询 qB(B_state, argsA)
Coordinate：B 查询 qA(A_state, argsB)
```

这不会形成递归，因为双方查询的都是屏障之前已经冻结的状态。

但以下循环不能在一次普通同步阶段中求解：

```text
A_final 依赖 B_final
B_final 又依赖 A_final
```

这种需求必须选择以下一种方式：

1. 把共同依赖抽成第三个基础能力 C；
2. 增加显式、有界的阶段，例如 `derive-0 → derive-1 → coordinate`；
3. 使用有终止条件的固定点迭代器，并由 Nyxia 强制最大轮数；
4. 转成 Act/Photon，在下一拍继续；
5. 如果二者本质上共享同一不可分割状态机，合并为一个 Lens。

默认实现只提供前两阶段，不自动执行固定点迭代。

## 7. Capability 与 Manifest

### 7.1 按能力解耦，不按 Lens ID 调用

Consumer 不应声明：

```yaml
calls:
  - org.tokmon.lens.textus
```

而应声明：

```yaml
provides_queries:
  - capability: text.select-messages
    request_schema: tokmon.text.select-messages.request.v1
    response_schema: tokmon.text.select-messages.response.v1
    deterministic: true
    default_timeout_ms: 10
    max_response_bytes: 1048576

consumes_queries:
  - capability: model.context
    cardinality: many
    required: false
    merge: priority_then_path
```

这样 Janus 依赖的是 `model.context` 能力，而不是 Enso 的具体实现。卸载 Enso 后可以由其他 Memory/RAG Lens 提供相同能力。

### 7.2 Provider 解析规则

Provider 解析必须由 Nyxia 在 BeatBoard 冻结时完成，而不是由 Consumer 动态遍历对象。建议支持：

| cardinality / merge | 语义 |
|---|---|
| `single` | 必须恰好一个 Provider；多个时拒绝发布 LightPath |
| `optional-single` | 零个或一个；缺失时 Consumer 使用降级路径 |
| `first` | 使用 LightPath 中第一个 Provider |
| `all` | 按确定顺序返回全部 Provider 结果 |
| `priority_then_path` | 先按显式优先级，再按 LightPath index |

同一个 capability、request schema 和 response schema 的兼容性应在 mount/reconcile 的 dark lane 中校验。

### 7.3 不应滥用 `optical_before/after`

因为所有 Provider 的基础状态会在 Coordinate 前冻结，所以普通 query 不要求 Provider 在 Consumer 之前。`optical_before/after` 只应用于确实存在的阶段性语义、最终 Surface 优先级或 Act arbitration，而不应成为通用隐藏调用依赖。

## 8. 因果异步协同

### 8.1 哪些协同不可同步

以下行为必须保持异步：

- 调用大模型；
- 文件读取、写入、移动和删除；
- Git stage、commit、merge、rebase；
- 启动、取消或等待进程/PTY/Worker；
- HTTP、MCP、LSP 和其他外部连接；
- Secret 创建、绑定、读取、消费和轮换；
- Lens 挂载、卸载和 reconcile；
- 子 Agent/子 Ray 创建、消息、join 和 cancel；
- 用户审批和不可逆操作；
- 可能超过拍内查询 deadline 的长任务；
- 需要重试、退避、恢复或 outcome-unknown 处理的行为；
- 会改变规范状态或未来行为的任何操作。

即使某项操作从操作系统角度“只是读取”，只要它访问可变化的外部资源，例如文件或网络，也不应伪装成纯同步查询。它应通过 Act 形成可审计的输入和结果事实。

### 8.2 标准异步链路

```text
Lens A Coordinate
    ↓ propose Act
SurfaceSnapshot.proposals
    ↓ Nyxia arbitration
act.proposed Photon
    ↓ schema / target / epoch / generation / policy / approval
act.admitted Photon
    ↓
唯一目标 Lens B.refract()
    ↓
外部作用或规范状态变化
    ↓
业务结果 Photon + act.completed/failed/rejected
    ↓
下一拍 PhotonWindow
    ↓
Lens A、Lens C、UI、审计 Lens 共同观察
```

这里的“异步”主要指因果阶段跨拍，不要求实现一定创建后台线程。目标 `refract()` 可以在当前线程完成，但调用方 Lens 不在自己的同步栈中直接得到返回值；它通过新 Photon 在后续拍观察结果。

### 8.3 异步请求的关联

Act 和结果 Photon 至少应通过以下字段建立关联：

| 字段 | 用途 |
|---|---|
| `ray` | 归属同一因果光流 |
| `Act.id` | 唯一请求身份 |
| `caused_by_act` | 结果 Photon 指向原因 Act |
| `idempotency_key` | 重试时避免重复副作用 |
| `epoch` | 绑定提出 Act 时的 LightPath epoch |
| `target` | 目标 Lens 身份 |
| `generation` | 目标挂载代际 |
| deadline/timeout | 限制等待时间 |

业务 Lens 不应通过解析自然语言猜测某个 Photon 是否属于某个请求，而应使用结构化关联字段。

### 8.4 异步失败和恢复

异步协同必须显式覆盖：

- schema 不匹配：`act.rejected`；
- 没有唯一目标：`act.rejected`；
- policy deny：`act.rejected`；
- 等待审批：保持明确的 approval-required/pending 状态，而不是重复执行；
- 执行失败：`act.failed`，携带受控错误分类；
- daemon 在开始后终止：恢复为 `act.outcome-unknown`，不得默认重试不可逆操作；
- deadline 到期：取消 Beam 或记录 timeout；
- 目标 generation 已离开 active path：拒绝新执行，保留已提交历史；
- 幂等重试：相同 idempotency key 返回既有结果或安全重放。

### 8.5 不要同步等待 Photon

Lens 的 `coordinate()` 不应这样工作：

```cpp
auto act = propose_process_exec(...);
while (!photon_store.has_result(act.id)) {
  wait();
}
```

这会占住当前拍，阻止负责产生结果的后续调度，并重新制造死锁。正确做法是：

```text
本拍提出 Act并结束
→ Nyxia 执行目标
→ 写入结果 Photon
→ 下一拍调用方观察结果并继续
```

## 9. 同步还是异步：判定规则

### 9.1 快速决策表

| 问题 | 是 | 否 |
|---|---|---|
| 是否会改变文件、进程、网络、模型、Secret、挂载或用户可见规范状态？ | 异步 Act | 继续判断 |
| 是否需要审批、重试、取消、恢复或幂等？ | 异步 Act | 继续判断 |
| 输入是否仅为当前 Photon 前缀、冻结状态和结构化参数？ | 继续判断 | 异步 Act 或重新建模 |
| 是否能在严格 timeout/CPU/内存/输出预算内完成？ | 同步 query | 异步 Act |
| 是否需要在 query handler 中调用另一个 Lens？ | 拆分阶段或异步 | 同步 query |
| 结果是否需要成为未来规范行为和恢复依据？ | 提交最小 Photon | 可仅留在 BeatBoard |

### 9.2 典型分类

| 场景 | 机制 | 原因 |
|---|---|---|
| 读取当前工具目录 | `get()` | 已派生、只读 |
| 按光标位置查询 AST | `query()` | 带参纯计算 |
| 根据 token budget 选择上下文 | `query()` | 只读、可缓存 |
| 验证工具参数 Schema | `query()` | 确定性纯计算 |
| 读取实际文件内容 | Act → Cove | 外部文件可能变化，需要事实结果 |
| 写文件 | Act → Cove | 外部副作用 |
| 运行命令 | Act → Styx | 进程副作用、超时、取消 |
| 调用模型 | Act → Rhea | 网络/模型不确定性和计费 |
| 调用 MCP 工具 | Act → Iris | 外部连接和权限 |
| 查询已冻结的 policy preview | `query()` | 只读预判，不是 admission |
| 真正批准或拒绝 Act | Act/宿主 admission | 改变执行资格，需要审计 |
| 创建子 Agent | Act → Aya | 生命周期和资源变化 |

## 10. 代表性协同示例

### 10.1 Janus 组合 Textus、Enso、Techor 和 Rhea

Derive 阶段：

```text
Textus → model.messages、textus context、budget
Enso   → memory/RAG/skill context
Techor → model.tools
Iris   → external model.tools
Rhea   → model.catalog
```

Coordinate 阶段：

```cpp
auto messages = optical.get("model.messages", "active-ray");
auto contexts = optical.get_all("model.context");
auto tools = optical.get_all("model.tools");
auto model = optical.get("model.catalog", selected_model);

auto selected = optical.query("context.select", cbor::object({
    {"contexts", contexts},
    {"input_budget_tokens", input_budget},
    {"reserved_output_tokens", output_budget}
}));

surface.propose(make_model_call(messages, selected, tools, model));
```

这里的消息、上下文和工具组合是同步协同；真正调用模型仍是 `model.call` Act，由 Rhea 异步产生模型结果 Photon。

### 10.2 Syntax Lens 与 Completion Lens

```text
Derive：Syntax 构建 Frozen AST/符号索引
Derive：Completion 构建当前编辑意图基础状态
Freeze
Coordinate：Completion query("syntax.at-position", cursor)
Coordinate：Syntax query("completion.intent", file)（如确有需要）
```

双方都可以查询对方的冻结基础状态，但不能让各自最终结果无限互相依赖。

### 10.3 Techor 到 Styx 的进程执行

这不能改成同步 query：

```text
Techor 解析 model.tool-call
→ propose process.exec Act
→ Nyxia admission
→ Styx.refract 执行进程
→ process.stdout/process.stderr/process.completed Photon
→ 下一拍 Janus 观察结果
```

即使模型希望“马上得到 stdout”，也必须跨 Act/Photon 边界，因为命令可能阻塞、超时、失败、产生大量输出或修改 Workspace。

### 10.4 Clotho 工作流

Clotho 可以同步查询冻结的工具目录、策略预览或工作流图分析，但工作流节点执行必须异步：

```text
Clotho 选择可运行节点（同步纯计算）
→ workflow.step Act
→ workflow.step-dispatched Photon
→ Techor 解码目标 Act
→ 目标 Lens 执行
→ 终态 Photon
→ Clotho 下一拍推进 DAG
```

### 10.5 不允许的同步环

```text
A.query → B.query → C.query → A.query
```

应改为：

```text
A、B、C 各自 Derive 基础状态
→ Freeze
→ 一个 Coordinator 同时读取三者
```

或者：

```text
A 提出 Act
→ B 执行并发 Photon
→ C 下一拍继续
```

## 11. 临时状态与 Photon 持久化边界

### 11.1 默认不应 Photon 化的状态

以下内容通常只属于 BeatBoard、缓存、telemetry 或 artifact：

- AST 节点和语法 token；
- 高亮范围；
- 未采用的补全候选；
- 工具 Schema 编译缓存；
- RAG 初筛候选和向量距离；
- token 预算中间计算；
- UI 布局和渲染中间状态；
- 重复轮询心跳；
- 进程读取循环的机械进度；
- query cache 和 Provider 内部索引。

生命周期默认为：

```text
Beat 开始创建 → Beat 内冻结使用 → Beat 结束释放
```

### 11.2 应提交 Photon 的结果

如果信息满足以下任一条件，应提交最小充分 Photon：

- 改变后续规范行为；
- 是外部副作用的已观察结果；
- 是恢复未完成操作所必需；
- 是审批、权限、目标代际或幂等证据；
- 是用户明确要求保留的结果；
- 是不可从确定输入和版本重新派生的重要决策。

例如，不需要保存全部补全候选，但应保存用户最终接受了哪个补全，以及由此产生的文件写入 Act 和结果。

### 11.3 同步查询审计

默认不把每次查询的完整请求和响应写成 Photon。建议写入有界 telemetry/trace：

```text
ray / beat
consumer lens + generation
provider lens + generation
capability
request schema
request hash
response hash
cache hit
duration
status
```

只有当查询结果成为外部 Act 的关键决策依据时，才在 Act provenance 中附带必要的 capability、provider generation、request/response hash；不复制大体积响应。

## 12. 确定性、缓存与性能

### 12.1 查询缓存键

同拍缓存建议使用：

```text
CacheKey =
  beat_id
  + path_hash
  + provider_id
  + provider_generation
  + capability
  + request_schema
  + canonical_request_hash
```

缓存只在当前 BeatBoard 生命周期内有效，除非 Provider 明确声明输入版本和跨拍缓存策略。

### 12.2 规范化请求

在计算 request hash 前必须对 CBOR map 进行 canonical encoding，避免字段插入顺序导致不同缓存键或回放哈希。

### 12.3 预算

每项 capability 应声明默认和最大预算：

```yaml
default_timeout_ms: 10
max_timeout_ms: 100
max_request_bytes: 262144
max_response_bytes: 1048576
max_concurrent_queries: 4
cache: per_beat
```

预算超过时返回结构化错误，不得自动退化成无界后台工作。

### 12.4 慢查询升级为异步 Act

如果一次查询需要扫描巨大仓库、调用模型或执行长时间索引，应提供异步能力：

```text
同步：workspace.symbols.query（只查已有索引）
异步：workspace.reindex Act（更新索引）
```

同步查询消费现有冻结索引，异步 Act 负责构建或刷新索引。这种读写分离能保持拍内延迟稳定。

## 13. 错误与降级语义

`optical.query()` 至少应区分：

| 错误 | 含义 | 建议处理 |
|---|---|---|
| `provider_not_found` | 当前 LightPath 没有能力提供者 | required 能力终止；optional 能力降级 |
| `ambiguous_provider` | `single` 能力出现多个提供者 | LightPath 发布阶段拒绝 |
| `schema_mismatch` | 请求或响应不符合 manifest | 隔离 Provider 结果并记录诊断 |
| `deadline_exceeded` | 查询超时 | 使用降级结果或转异步 Act |
| `budget_exceeded` | 请求、响应、CPU 或内存超预算 | 截断只允许由 Schema 明确声明 |
| `provider_failed` | Provider 抛出或返回错误 | 不影响其他 Provider；记录 provenance |
| `stale_generation` | Provider 已不属于当前 BeatBoard | 拒绝调用，不重新解析到新代 |
| `recursive_query_denied` | Provider 尝试嵌套跨 Lens 查询 | 立即拒绝并记录诊断 |
| `nondeterministic_result` | 回放哈希不一致 | 标记实现缺陷或降级为异步事实来源 |

Optional 能力必须有明确默认值，不能因为提供者被拆卸而留下悬空引用。

## 14. 生命周期与可拆卸性

### 14.1 BeatBoard 固定 generation

BeatBoard 冻结时记录 Provider 的：

- Lens ID；
- artifact hash；
- mount epoch；
- generation；
- capability schema；
- LightPath index。

当前拍持有 LightPath snapshot 和必要的 provider lease。reconcile 可以准备新 path，但不能让当前 query 突然改投新 generation。

### 14.2 卸载语义

卸载某 Provider 后：

- 已经开始且持有旧 BeatBoard lease 的拍可以按 drain 策略完成；
- 新拍不再解析到旧 Provider；
- optional Consumer 自动降级；
- required Consumer 导致候选 LightPath 在 dark lane 中被拒绝，或 Ray 明确失败；
- 不删除历史 Photon；
- 不保留跨拍 Provider 对象指针。

### 14.3 Worker Lens

Worker Provider 可以通过有界 IPC 实现同样的 query contract：

```text
Nyxia → query request CBOR → Worker
Worker → response CBOR → Nyxia
```

语义仍是同步返回，但必须有更严格 timeout、消息大小和取消机制。高频低延迟能力宜使用进程内可信 Provider或宿主索引服务；不能因为 Worker IPC 较慢就允许绕过查询预算。

## 15. ABI 与兼容演进

### 15.1 不直接修改既有 C++ 虚表

给现有跨 DLL `ILens` 直接增加虚函数可能破坏二进制 ABI。建议使用可选扩展：

```text
tokmon_lens_get_extension("org.tokmon.optical-query.v1")
```

或者在已有 C ABI 表尾增加由 `struct_size` 保护的可选函数指针：

```cpp
struct TokmonLensApiV1 {
  std::uint32_t struct_size;
  // existing members...
  tokmon_lens_derive_fn derive;
  tokmon_lens_coordinate_fn coordinate;
  tokmon_lens_query_fn query;
};
```

旧 Lens 没有这些成员时：

- 现有 `view()` 作为兼容 Derive；
- 不注册 query capability；
- Coordinate 默认为空；
- 现有 proposal 仍可参与最终 arbitration。

### 15.2 渐进迁移

第一阶段无需一次改造所有 Lens。只有需要同拍组合的 Lens 实现新扩展：

1. Textus、Enso、Techor 注册现成数据；
2. Janus 实现 Coordinate 并消费这些数据；
3. 代码分析类 Lens 再实现带参 query；
4. UI、telemetry 和纯观察 Lens 可保持旧 `view()`。

## 16. 对 Nyxia 的最小实现改造

### 16.1 新增类型

```text
BeatId
BeatBoardBuilder
BeatBoardSnapshot
FrozenLensState
OpticalContext
OpticalQueryRegistry
OpticalQueryRequest/Response
QueryBudget
QueryTrace
```

### 16.2 改造 `RayTracingEngine::view()`

伪代码：

```cpp
Result<SurfaceSnapshot> RayTracingEngine::view(const RayId& ray) {
  auto window = read_photon_window(ray);
  auto path = path_.snapshot();

  BeatBoardBuilder board(path, window);
  SurfaceSnapshot surface;

  // Phase 1: Derive
  for (const auto& mounted : path->lenses) {
    auto builder = host_builder(mounted);
    mounted.lens->view(window, builder);       // v1 compatibility
    mounted.optional_derive(window, board);   // v2 extension
    surface.merge(builder);
  }

  auto frozen = board.freeze(surface);

  // Phase 2: Coordinate
  for (const auto& mounted : path->lenses) {
    if (!mounted.has_coordinate()) continue;
    OpticalContext optical(frozen, mounted.identity());
    auto builder = host_builder(mounted);
    mounted.coordinate(window, optical, builder);
    surface.merge(builder);
  }

  return deterministic_finalize(surface);
}
```

### 16.3 保持单 Act 副作用边界

该方案不要求修改 `ActPipeline::admit()` 和单目标 `refract()` 的核心边界。Coordinate 可以增加 proposal，但 Nyxia 每次仍只提交一个经过 admission 的 Act。

现有 `proposals.front()` 是隐式光路优先级，后续可独立演进为显式、确定的 proposal arbitration；这不是引入有限同步协同的前置条件。

## 17. 安全与资源约束

即使 query 在语义上纯净，宿主仍应强制：

- request/response schema；
- request/response 字节数；
- deadline；
- 每拍调用次数；
- 每 Consumer/Provider 配额；
- recursion depth = 1；
- generation 和 epoch 匹配；
- canonical request hash；
- exception 隔离；
- 敏感字段禁止进入普通 query response；
- QueryTrace 自动脱敏。

对于同进程 C++ Lens，完全禁止其绕过宿主访问 OS 仍依赖更完整的隔离机制；但同步协同 API 本身不应提供新的文件、网络、进程或 Secret 句柄。

## 18. 测试与验收标准

### 18.1 功能测试

- Consumer 可以用复杂 CBOR 参数查询 Provider；
- 同一请求同拍多次调用命中缓存；
- A、B 可以分别查询对方的 Frozen State；
- Provider 位于 Consumer 之后时仍可查询，因为 Derive barrier 已完成；
- optional Provider 卸载后 Consumer 正确降级；
- required Provider 缺失时候选 LightPath 被拒绝或返回明确错误；
- Worker Provider 与 in-process Provider 具有相同 Schema 语义。

### 18.2 确定性测试

- 相同输入前缀、path hash、generation 和请求得到相同 response hash；
- map 字段顺序不同但语义相同时产生相同 request hash；
- Surface 合并顺序稳定；
- reconcile 并发发生时当前 BeatBoard 不切换 Provider generation；
- 回放时 QueryTrace 能定位提供者和输入版本。

### 18.3 约束测试

- query handler 尝试嵌套查询时被拒绝；
- timeout 后返回 `deadline_exceeded`；
- 超大请求和响应被拒绝；
- Provider 抛异常不会终止其他 Lens 的 Derive/Coordinate；
- query 不能获得 `RefractionBeam`；
- query 不能发射 Photon 或提出 Act；
- stale generation 不能被重新路由到新 Provider。

### 18.4 异步协同测试

- 文件、进程、模型和外部调用无法通过 query capability 暴露；
- Act 提出、准入、开始和终态 Photon 的因果关联完整；
- 调用方只在下一拍观察异步结果；
- 不可逆 Act 在 outcome unknown 时不自动重复；
- Lens 不会在 Coordinate 中同步等待结果 Photon。

### 18.5 性能基线

至少测量：

- 19/20 个 Lens 两阶段空拍开销；
- 100、1000、10000 次同拍缓存查询；
- 大型 Frozen State 构建和释放；
- in-process 与 Worker query 延迟分布；
- Textus/Enso/Techor 组合后 Janus 重复计算减少量；
- 未写入 PhotonStore 的机械派生数据体积；
- timeout 和失败 Provider 对整拍尾延迟的影响。

## 19. 分阶段落地计划

### 阶段 A：只读 Surface 索引

- 把第一遍 `view()` 的 contribution 建成冻结索引；
- 提供 `get()`、`get_all()`；
- 增加 generation、path index、schema、priority provenance；
- 暂不支持参数化 handler。

目标：让 Janus 能读取 Textus、Enso、Techor 已经发布的 Surface。

### 阶段 B：Coordinate 扩展

- 增加可选 Coordinate ABI；
- 把 Janus 的 `model.call` proposal 移到 Coordinate；
- 保留旧 Lens 单阶段兼容；
- 增加两阶段 trace 和错误隔离。

目标：真正接通 `messages + context + tools + model catalog → Janus → Rhea`。

### 阶段 C：参数化 SOQ

- 增加 Provider capability、request/response schema；
- 增加 `query()`、per-beat cache、timeout、大小限制；
- 首先实现 `syntax.at-position`、`context.select`、`tool.validate-arguments` 等纯能力。

目标：消除高价值场景中的重复计算和机械 Photon。

### 阶段 D：异步边界强化

- 为 SDK 增加同步/异步能力 lint；
- 禁止 manifest 把副作用权限暴露为 query capability；
- 完善 Act 关联、超时、恢复和 outcome-unknown 测试；
- 文档化各官方 Lens 的 sync capabilities 与 Act capabilities。

### 阶段 E：优化与多语言

- Worker query IPC；
- 大型 Frozen State 的宿主句柄或共享内存优化；
- 热点 query profiling；
- 跨拍只读索引缓存及版本验证；
- 必要时增加显式有界多阶段，但不开放递归网状调用。

## 20. 风险与权衡

### 20.1 两阶段会增加固定调度成本

每拍多一次 LightPath 遍历，但 Coordinate 只对实现扩展的 Lens 调用，且可通过缓存减少重复解析。应以 benchmark 决定是否对无协同 Ray 跳过第二阶段。

### 20.2 “纯查询”仍可能很慢

纯函数不等于低成本。必须使用 deadline、输出大小、per-beat cache 和慢能力异步化，不能只依赖开发者自觉。

### 20.3 Frozen State 可能占用大量内存

大型 AST、索引和 RAG 候选应使用拍内共享表示、结构共享或宿主句柄，避免为每个 Consumer 复制。响应仍应是有界结构化数据。

### 20.4 能力设计可能过细或过粗

过细会造成大量查询和 Schema 管理，过粗会把多个职责重新塞进一个万能能力。Capability 应围绕稳定语义，而不是围绕某个 Lens 的私有方法逐一映射。

### 20.5 同步协同不能解决真正循环状态机

如果 A 和 B 的最终状态必须互相反复修正直到收敛，简单两阶段不能自动解决。此时应明确选择 Coordinator、有限迭代或跨拍工作流，而不是暗中放开递归调用。

## 21. 规范性规则汇总

以下规则建议成为 SDK 与运行时的正式约束：

1. Lens **不得**持有或直接调用另一个 Lens 的实现对象；
2. 同拍跨 Lens 协同**必须**通过 Nyxia 提供的 BeatBoard/SOQ；
3. query 请求**可以**携带复杂结构化参数；
4. query handler **必须**只读取 Frozen State、请求参数和明确的确定性宿主函数；
5. query handler **不得**获取 OpticalContext、RefractionBeam 或可写 PhotonStore；
6. query handler **不得**执行跨 Lens 嵌套查询；
7. 所有 query **必须**绑定当前 epoch、generation 和 schema；
8. 所有 query **必须**有 deadline、大小和调用次数预算；
9. 所有 Provider 解析和多提供者合并**必须**确定；
10. 外部 I/O、规范状态变化、审批、恢复和长任务**必须**使用 Act；
11. 异步结果**必须**通过结构化 Photon 与 Act 关联，并在后续拍观察；
12. Lens **不得**在 Coordinate 中阻塞等待自己的 Act 结果；
13. 机械派生状态默认**不得**写入 PhotonStore；
14. 影响未来规范行为、恢复或审计的最小充分结果**必须**Photon 化；
15. 卸载 Provider 后新拍**不得**继续调用旧 generation；
16. 老 Lens 不实现扩展时**必须**保持兼容运行。

## 22. 最终结论

`tokmon-n` 不需要在“完全隔离、只能跨拍 Photon”和“任意 Lens 互调、形成网状插件系统”之间二选一。更合适的中间方案是：

```text
同一 Photon 前缀
    ↓
所有 Lens 独立 Derive
    ↓
冻结 BeatBoard
    ↓
通过 Nyxia 进行一层、只读、带参、受预算的同步能力查询
    ↓
形成 Surface 和 Act proposal
    ↓
副作用通过单目标 Act 执行
    ↓
结果以 Photon 进入下一拍
```

它允许复杂同步计算，但不允许任意同步副作用；允许多个 Lens 共享派生能力，但不允许对象级递归调用；允许临时高密度数据留在内存中，但要求真正影响因果历史的结果进入 Photon。

这一区分可以概括为：

> **有限同步协同用于组合当前认知，因果异步协同用于改变系统与世界。**
