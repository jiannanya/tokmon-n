# Tokmon 内置透镜完整功能设计

> 架构题记：**A Lens to Them All**
>
> 文档性质：目标功能规范与验收基线
>
> 语言与宿主：C++20
>
> 桌面显像：Slint
>
> 上位设计：[DESIGN.md](./DESIGN.md)
>
> 能力来源：[20.md](./20.md)

## 1. 文档目的

本文把 `20.md` 中的产品能力转写为可以设计、实现、组合、集成测试和验收的内置透镜功能规范。本文不讨论历史架构如何替换，只描述最终系统必须具备的行为。

Tokmon 的“二十”采用以下口径：

- Nyxia 是唯一静态微内核，提供所有透镜共同依赖的运行规则；
- Ignis 至 Termon 是十九个可在运行时组合、换代和拔出的正式内置透镜；
- Calculator 是 SDK 和端到端测试使用的参考透镜，不计入正式十九镜。

本文中的“完成”指真实能力闭合，而不只是存在 C++ 类型、manifest、Fact 名称或成功测试桩。例如：Rhea 完成意味着至少一个真实远端模型适配器能够流式工作；Iris 完成意味着真实 MCP/LSP 进程可以连接和交互；Cista 完成意味着凭据确实存放在操作系统安全存储中。

当前代码完成度单独记录在 [IMPLEMENTATION-REPORT.md](./IMPLEMENTATION-REPORT.md)。本文所有 `MUST`、功能编号和验收条目都是目标要求，不能被实现报告反向削弱。

## 2. 全局功能边界

### 2.1 唯一语义链

所有业务能力都服从：

```text
committed Fact / Photon
        ↓
       Lens.view
        ↓
Surface contribution + proposed Act
        ↓
Techor normalization → Fallen admission → Cista binding → Styx envelope
        ↓
target Lens.refract
        ↓
PhotonDraft → Nyxia append gate → new committed Fact / Photon
```

透镜之间不得通过对象引用、服务定位器或隐藏回调交换业务状态。跨透镜信息必须先成为 committed Photon，或成为当拍只读 Surface contribution。现实 I/O 只能发生在获得 Beam 的 `refract` 边界。

### 2.2 因果光子流只追加

- committed Photon 禁止编辑、覆盖、撤销或删除；
- 拒绝、取消、补偿、重试、恢复、回滚和配置换代均追加新 Photon；
- “撤销文件修改”是新的反向 `fs.write` Act，不是修改旧记录；
- “回滚透镜”是发布更高 epoch 的旧 artifact generation，不是把 epoch 指针改回过去；
- 派生缓存、索引和 checkpoint 可以删除重建，但不能冒充事实源；
- SQLite 必须以 trigger 和连接权限物理拒绝 committed 表的 `UPDATE`/`DELETE`。

### 2.3 工程基线

- C++ 标准固定为 C++20；
- 可预期失败统一使用 `tl::expected<T, Error>`；
- C++ 进程统一通过 `spdlog` 输出结构化日志；
- Tokmon 自有运行配置、透镜 manifest、策略和工作流使用 YAML；
- YAML 未知字段、重复 key、类型错误和非法路径必须报错，不能静默忽略；
- Node.js 与 CPython 透镜运行在独立 Worker 中，可以使用锁定并离线物化的 npm/PyPI 依赖；
- Termon 是唯一链接 Slint 的主程序，Slint property/model 只是 UiSurface 缓存。

### 2.4 用户级与项目级目录

```text
用户级：<user-home>/.tokmon/
项目级：<workspace>/.tokmon/
```

配置按下列顺序折叠，后者只能在上位信任边界内细化或收紧：

```text
built-in defaults
→ <user-home>/.tokmon/config.yaml
→ <workspace>/.tokmon/config.yaml
→ <workspace>/.tokmon/local.yaml
→ explicit CLI overrides
```

期望光路位于用户级或项目级 `.tokmon/light-path.yaml`。文件变化只追加 `config.light-path-observed`；Ignis 验证成功且 Nyxia 提交新 mount epoch 后，新的 LightPath 才生效。

### 2.5 公共 Act 生命周期

```text
act.proposed
├─ act.denied
├─ approval.requested → approval.granted / denied / expired
└─ act.admitted
   ├─ act.started → act.succeeded
   ├─ act.started → act.failed
   ├─ act.started → act.outcome-unknown
   └─ act.cancel-requested → act.cancelled / act.completed-after-cancel
```

每个 Act 至少携带：`act_id`、`ray_id`、`kind`、`schema`、`target`、`epoch`、`target_generation`、`idempotency_key`、`impact_class`、`deadline`、`caused_by`。审批必须绑定规范化 Act hash、epoch 和目标 generation。

### 2.6 公共错误模型

```yaml
error:
  domain: iris
  code: external.timeout
  message: "MCP call exceeded deadline"
  retryable: true
  outcome_known: true
  caused_by_act: "act-..."
  details_ref: "blob:sha256:..."
```

- 边界异常只在最外层 adapter 转换为结构化 `Error`；
- 错误正文和第三方响应过大时存入 Chora Blob，Photon 只保留摘要与引用；
- 错误日志不是恢复依据，恢复只读取 Photon；
- Secret 明文不得进入 `Error`、Photon、Surface、日志、UI、artifact metadata 或诊断包。

## 3. 可随时组合与热插拔的透镜体系

### 3.1 优雅性来自统一形状

Tokmon 不把 MCP、模型、工作流、记忆、工具、终端等能力硬编码为微内核服务。每项能力都以同一种形状进入系统：声明观察哪些 Photon、贡献哪些 Surface、提出或接受哪些 Act。新能力不要求修改 Nyxia 主循环，也不要求其他透镜增加对它的对象依赖。

这种统一形状带来四项核心性质：

1. **可组合**：透镜只通过显式契约叠加贡献，可以按用户、项目、任务和 ray 选择组合；
2. **可替换**：相同契约的新 generation 可以在候选光路验证后原子接管；
3. **可拔出**：拔镜后下一拍立即停止新 Surface/Act 贡献，历史仍由 Photon 完整保留；
4. **可重建**：业务状态来自只追加 Photon，新 generation 不需要窃取旧对象内存即可恢复显像。

### 3.2 组合层级

LightPath 由四层期望组合折叠得到：

```text
signed bootstrap lenses
→ user .tokmon/light-path.yaml
→ project .tokmon/light-path.yaml
→ explicit run/ray overlay
```

- 用户级组合给出跨项目默认能力；
- 项目级组合增加项目专用 instruction、工具、LSP、模型路由或更严格策略；
- run/ray overlay 只在既有信任上界内临时选镜，例如为一次数据分析装入 Python RAG 透镜；
- 任一层都不能覆盖更高层的 deny、信任根和 Secret 使用边界。

示例：

```yaml
api: tokmon.light-path/v1
lenses:
  - lens_id: org.tokmon.lens.rhea
    artifact: org.tokmon.lens.rhea@2.1.0
    order: 400
  - lens_id: org.example.lens.company-rag
    artifact: sha256:8a...
    runtime: cpython
    order: 850
    config_ref: project-rag
overlays:
  review:
    enable: [org.example.lens.security-review]
    disable: [org.example.lens.company-rag]
```

### 3.3 组合合法性

候选 LightPath 必须同时满足：

- 每个 Lens ID 只有一个 active generation；
- `(ActKind, schema)` 至多命中一个目标 generation；
- 多个透镜可以贡献同一可合并 SurfaceChannel，但 contribution 必须有稳定 source/priority/trust；
- manifest 的 PhotonPattern、SurfaceChannel、ActPattern、权限与资源上限完整；
- 依赖是“需要某种契约/Surface/Act”，不得变成对具体 C++ 对象的调用；
- 缺失可选能力时产生降级显像；缺失必需契约时拒绝发布候选光路。

### 3.4 原子装入、换代与拔出

```text
desired composition observed
→ resolve immutable artifacts and exact runtimes
→ verify manifest/signature/dependency/permission
→ construct candidate generations
→ dark-lane view + synthetic refract + replay test
→ build isolated E+1 LightPath
→ append mount.epoch-committed
→ atomic publish complete E+1 snapshot
→ stop routing new Beams to E generations
→ bounded afterglow → stop → unload
```

发布前失败不会污染当前光路。发布后若发现问题，只能把其他 generation 作为 E+2 发布。任意 engine step 只使用一次捕获的不可变 LightPathSnapshot，不会在半拍看到混合 epoch。

### 3.5 复合能力也是组合而非大一统实现

完整产品能力可以由多镜协作形成。例如“修改代码”由 Textus/Enso 提供上下文，Rhea/Janus 产生意图，Techor 解码，Fallen/Cista/Styx 收紧边界，Cove 执行并观察，Chora/Tracket/Nota 保存与诊断，Snow/Termon 显像。任何参与者都可在保持契约的前提下换代，而整条能力链无需重写。

常用组合可以保存为 YAML profile，但 profile 只是期望光路片段，不是新的特殊运行机制：

```yaml
profiles:
  coding:
    require: [iris, rhea, janus, textus, enso, techor, fallen, cista, styx, cove]
  automation:
    require: [clotho, aya, techor, fallen, styx, chora, tracket]
  headless:
    require: [snow]
    optional: [termon]
```

### 3.6 组合与热插拔验收

- 随机装入、换代、重排和拔出十九镜，不出现横向对象依赖断裂；
- 同一 PhotonWindow 在不同合法 LightPath 下得到各自确定的 Surface；
- 新 generation 能仅靠 manifest、配置和 PhotonWindow 重建状态；
- 拔出任意非必需透镜后，其 contribution 下一拍归零，其他镜继续工作或明确降级；
- Node.js/CPython 新旧 worker 可在 handoff 窗口共存，但发布后新 Beam 只进入新 generation；
- 组合变化、验证结果、发布、afterglow 和失败全部形成新 Photon。

## 4. 二十组件及复合能力

### 4.1 组件总览

| # | 组件 | 类型 | 完整功能结果 |
| ---: | --- | --- | --- |
| 1 | Nyxia | 微内核 | 事实提交、光路发布、Beam、监督、恢复和原子换代 |
| 2 | Ignis | 正式透镜 | artifact 验证、依赖解析、期望组合协调和热替换 |
| 3 | Lemon | 正式透镜 | 有界实时传输、订阅、广播、cursor、背压和重连 |
| 4 | Iris | 正式透镜 | MCP、LSP、本地扩展协议和 Worker IPC 的统一桥接 |
| 5 | Rhea | 正式透镜 | 多模型提供方、流式响应、限额、重试和故障转移 |
| 6 | Janus | 正式透镜 | 默认单 Agent 推理—行动—观察闭环 |
| 7 | Clotho | 正式透镜 | YAML DAG、条件、并行、批处理、重试和恢复 |
| 8 | Aya | 正式透镜 | 子运行、上下文继承、worktree 隔离、协同与结果合并 |
| 9 | Textus | 正式透镜 | Prompt/ModelSurface 组装、token 预算、裁剪和摘要 |
| 10 | Enso | 正式透镜 | instruction、SKILL.md、长期记忆、向量索引和 RAG |
| 11 | Techor | 正式透镜 | 通用工具目录、schema 解码、动态工具和 Code Mode |
| 12 | Styx | 正式透镜 | 本地强沙箱、远程容器、PTY、配额和进程树控制 |
| 13 | Fallen | 正式透镜 | 权限策略、根策略、审批瀑布和内容安全判定 |
| 14 | Cista | 正式透镜 | OS 凭据库、SecretRef、短时绑定和跨出口脱敏 |
| 15 | Chora | 正式透镜 | SQLite WAL、版本化 KV、加密 Blob、归档和备份 |
| 16 | Tracket | 正式透镜 | 因果验证、Raw Trace Vault、R0—R3 回放和导出 |
| 17 | Nota | 正式透镜 | spdlog 汇聚、OpenTelemetry、指标、Span 和实时诊断 |
| 18 | Cove | 正式透镜 | 工作区树、文件监听、Git 快照、前后镜像和 artifact |
| 19 | Snow | 正式透镜 | headless CLI、stdio、本地协议、doctor 和脚本入口 |
| 20 | Termon | 正式透镜 | Slint Workbench、会话、轨迹、上下文、审批和诊断界面 |

### 4.2 `20.md` 复合能力的落点

| 复合能力 | 主透镜 | 协作透镜 | 必须闭合的结果 |
| --- | --- | --- | --- |
| LSP Bridge | Iris | Cove、Techor、Lemon、Nota | 按语言启动服务、同步文档、完成/跳转/诊断、退出回收 |
| File Watcher | Cove | Nota、Textus、Chora | debounce、聚合、Git 状态和前后镜像形成可追溯事实 |
| MCP Host | Iris | Techor、Fallen、Cista、Styx | client/server、发现、schema 校验、调用、取消和重连 |
| Prompt Weaver | Textus | Enso、Techor、Rhea | 多源上下文在目标模型窗口内形成稳定 ModelSurface |
| Skill Loader | Enso | Ignis、Chora、Textus | 渐进加载、缓存失效、来源标注和按需注入 |
| Reasoning Stream | Rhea | Lemon、Textus、Janus | reasoning/content/tool-call 分流、流式呈现和最终汇总 |
| DAG Engine | Clotho | Janus、Aya、Fallen | YAML 工作流确定推进、并行汇合和崩溃恢复 |
| Agent Fork | Aya | Textus、Cove、Chora | 独立子 ray/worktree、预算继承、协作和安全合并 |
| Code Executor | Techor | Fallen、Cista、Styx、Cove | 代码解析为结构化 Act，在隔离中执行并回注结果 |
| Permission Guard | Fallen | Cista、Styx、Termon/Snow | deny-first、最小权限、人工审批和不可绕过准入 |
| Sanitizer | Cista | Fallen、Textus、Nota | 输入/输出/日志/artifact/诊断包统一脱敏 |
| Sandbox Manager | Styx | Techor、Fallen、Cista | OS/远程后端、PTY、CPU/Mem/Net/File 配额和回收 |
| OTel Exporter | Nota | Lemon、Cista | Span/Metrics/Logs 有界导出，失败不阻塞事实提交 |
| Trace Vault | Tracket | Chora、Cista | 只追加因果链、压缩原始轨迹和分级回放 |
| Session Store | Chora | Textus、Tracket | 会话、KV、Blob、checkpoint、备份和恢复 |
| CLI Runner | Snow | Lemon、Nota | human/machine/stdio 三种模式、doctor 和 CI 稳定退出码 |
| Workbench UI | Termon | Snow、Tracket、Nota、Ignis | 可视化会话、轨迹、上下文、审批、Lens 与诊断 |

## 5. Nyxia：微内核

### 5.1 功能职责

- `NYX-F-001`：维护唯一 append gate，为 Photon 分配不可复用的单调 sequence，并提交 hash chain；
- `NYX-F-002`：从最后一个完整 mount epoch 构造不可变 `LightPathSnapshot`；
- `NYX-F-003`：签发绑定 `epoch + generation + Act` 的 BeamTicket，过期或拔镜后拒绝使用；
- `NYX-F-004`：驱动 RayTracingEngine 的 view、Act admission、折射、提交和自然停机；
- `NYX-F-005`：监督所有线程、句柄、socket、watcher、worker 和子进程，统一 deadline/stop；
- `NYX-F-006`：完成新 LightPath 原子发布、旧 generation afterglow 和资源回收；
- `NYX-F-007`：崩溃恢复时验证数据库尾部、重建光路和活动 ray，并标记结果未知的在途 Act；
- `NYX-F-008`：提供 C ABI、Node.js/CPython Worker Protocol 和受限 Host API。

### 5.2 输入与输出

| 输入 | 用途 |
| --- | --- |
| `system.bootstrap-requested` | 启动并加载最小恢复光路 |
| `mount.reconcile-requested` | 构造并验证候选 LightPath |
| `act.proposed/admitted` | 推进固定 Act 管线 |
| `ray.cancel-requested` | 向对应 Beam/worker 传播 stop |
| `system.shutdown-requested` | 有界停止并落盘终态 |

| 输出 Photon | 语义 |
| --- | --- |
| `mount.epoch-committed` | 新光路事实已持久化，可以发布 |
| `lens.afterglow-started/completed` | 旧 generation 进入/完成回收 |
| `act.started/succeeded/failed/outcome-unknown` | Act 的规范终态 |
| `ray.darkened` | 当前 ray 无提案、无在途 Beam，自然停机 |
| `system.recovered/degraded/stopped` | 宿主生命周期结果 |

### 5.3 配置与验收

`config.yaml` 的 `nyxia` 节至少包含：engine 单拍上限、Beam deadline、afterglow deadline、PhotonWindow 限额、worker heartbeat、shutdown deadline、数据库同步级别。所有上限必须有安全默认值和硬上限。

- 并发提交不产生重复 sequence、断链或半条 Photon；
- 候选验证失败时 active path 与 epoch 不变；
- 原子发布后没有新 Beam 进入旧 generation；
- 随机停止/崩溃测试后不存在无归属线程、句柄或子进程；
- committed Photon 的 SQL 更新和删除在物理层失败；
- C++、Node.js、CPython 等价参考透镜通过同一 golden ray。

## 6. Ignis：透镜组合与换代

### 6.1 功能职责

- `IGN-F-001`：解析 `lens.yaml`、`lens-lock.yaml`、签名、SBOM、schema bundle 和 artifact hash；
- `IGN-F-002`：解析透镜依赖、冲突、optical order、ActPattern 唯一性和权限变化；
- `IGN-F-003`：支持进程内 C++ C ABI generation、native worker、Node.js、CPython、WASM 和 Termon desktop 承载；
- `IGN-F-004`：观察用户级/项目级 `light-path.yaml`，计算 desired/current diff；
- `IGN-F-005`：在 dark lane 完成 ABI、manifest、schema、权限、runtime、依赖树、view 和合成折射测试；
- `IGN-F-006`：提出 mount/replace/unmount/reconcile Act，并由 Nyxia 原子发布；
- `IGN-F-007`：支持 R1 generation swap、R2 worker/desktop handoff，以及 rollback-as-new-epoch；
- `IGN-F-008`：缓存经过验证的不可变 artifact，禁止运行时联网改变依赖图。

### 6.2 Surface、Act 与结果

| Surface | 字段 |
| --- | --- |
| `diagnostic.light-path` | desired/current hash、epoch、diff、冲突、验证状态 |
| `ui.lenses` | id、版本、runtime、generation、权限、health、afterglow |

| Act | 结果 Photon |
| --- | --- |
| `lens.verify.v1` | `lens.candidate-verified/rejected` |
| `lens.reconcile.v1` | `mount.reconcile-requested` |
| `lens.mount/replace/unmount.v1` | `mount.epoch-committed` 或 `mount.rejected` |

### 6.3 换代与验收

```text
observe YAML → resolve immutable artifact → verify trust/dependencies
→ start candidate generation → dark-lane replay/test
→ build isolated LightPath → verify unique routes
→ request admission when permission expands
→ commit mount epoch → atomic publish
→ stop new routing to old generation → drain → stop → unload
```

Ignis 自身不能批准或发布自身；其候选由当前 Ignis 准备，最终步骤由 Nyxia 固定适配器完成。

- 半写 YAML、重复 watcher 通知和相同内容 hash 不产生重复换代；
- ABI/runtime/依赖 hash 不匹配时拒绝装入；
- Node.js/CPython 新旧 runtime 可在 handoff 窗口共存，但发布后新请求只进入新 worker；
- 权限扩大必须重新审批；权限缩小允许直接进入验证；
- 回滚保留完整历史，并产生更高 epoch。

## 7. Lemon：有界实时传输

### 7.1 功能职责

- `LEM-F-001`：提供带 schema id 的 typed frame conduit；
- `LEM-F-002`：支持单消费者、消费组、多消费者和显式广播；
- `LEM-F-003`：对每个 conduit 配置容量、单帧上限、批量上限和背压策略；
- `LEM-F-004`：维护 producer tail 与 consumer cursor，拒绝 cursor 倒退；
- `LEM-F-005`：支持模型 chunk、Worker progress、UI delta、遥测批次的顺序合批；
- `LEM-F-006`：断线后从 Chora durable cursor 追赶，检测 gap 并请求 snapshot；
- `LEM-F-007`：换代时建立 epoch bridge，旧队列排空后结束 afterglow。

### 7.2 传输策略

```yaml
lemon:
  conduits:
    model-stream:
      capacity_frames: 2048
      max_frame_bytes: 1048576
      overflow: block-producer   # block-producer | reject-new | coalesce
      delivery: ordered-at-least-once
```

`coalesce` 只能合并声明可合并的临时 frame，不能合并或改变 committed Photon。现实动作请求禁止使用丢弃策略。

`waveguide.send-frame.v1`、`advance-cursor.v1`、`subscribe.v1`、`unsubscribe.v1`、`reconnect.v1` 分别产生 frame、cursor、subscription 和 reconnect 结果 Photon。

### 7.3 验收

- 同一 conduit 内 frame 顺序稳定；
- 广播的每个 durable consumer 有独立 cursor；
- 慢消费者不会无限增加内存；
- 宕机重连不会静默越过 gap；
- 超大帧、错误 schema、非法 cursor 和未知 conduit 被结构化拒绝。

## 8. Iris：MCP、LSP 与异构协议桥

### 8.1 功能职责

- `IRI-F-001`：实现 MCP client，支持能力协商、工具/资源/提示发现、调用、取消、进度和重连；
- `IRI-F-002`：实现 MCP server，把当前允许公开的 Techor 工具和 Enso 资源显像为外部能力；
- `IRI-F-003`：实现 LSP client 生命周期：按语言启动、initialize、文档同步、请求、诊断、shutdown/exit；
- `IRI-F-004`：把 completion、hover、definition、references、rename、code action、format 和 diagnostics 归一化为本地 schema；
- `IRI-F-005`：对接 Nyxia Worker Protocol，使 Node.js/CPython 能力以同一 external catalog 进入系统；
- `IRI-F-006`：维护 endpoint、协议版本、远端 schema hash、health、latency 和 capability catalog；
- `IRI-F-007`：区分 timeout、disconnect、remote error、protocol error 与 outcome unknown。

### 8.2 输入、Surface 与 Act

| 输入 Photon | 用途 |
| --- | --- |
| `external.endpoint-configured` | 声明 MCP/LSP/worker endpoint 与信任来源 |
| `workspace.document-opened/changed/closed` | 驱动 LSP 文档同步 |
| `external.catalog-invalidated` | 重新发现远端能力 |

| Surface | 内容 |
| --- | --- |
| `model.tools` | 经 Techor/Fallen 可见性过滤后的外部工具 schema |
| `diagnostic.external` | connection、protocol、schema hash、health、latency |
| `workspace.diagnostics` | 带 document version 的 LSP diagnostics |

| Act | 结果 |
| --- | --- |
| `external.connect/disconnect.v1` | connection opened/closed/failed |
| `external.call.v1` | completed/failed/timeout/outcome-unknown |
| `lsp.request.v1` | completion/definition/rename 等规范结果 |
| `external.serve.v1` | MCP server listener ready/stopped |

### 8.3 安全与验收

- endpoint 使用 opaque ref；网络、stdio 子进程和工作目录权限必须显式声明；
- 远端说明、文档和错误正文永远作为 data，不能升级为 instruction；
- schema 漂移使旧 tool call 失效，并触发 catalog refresh；
- LSP edit 在应用前必须转换为 Cove Act、展示 Diff 并经过审批；
- 用真实 MCP fixture 和至少两个真实 LSP server 完成发现、调用、诊断、重连与关闭测试；
- 非幂等调用断线时不得自动宣称失败或成功。

## 9. Rhea：多模型网关

### 9.1 功能职责

- `RHE-F-001`：提供统一 provider adapter，首发至少覆盖 OpenAI、Anthropic、DeepSeek、Gemini 和本地 deterministic provider；
- `RHE-F-002`：显像模型目录、上下文窗口、能力、价格、速率限制、地区和健康度；
- `RHE-F-003`：把 provider 流统一解析为 reasoning、content、tool-call、usage 和 terminal event；
- `RHE-F-004`：支持取消、deadline、首 token 超时、空闲流超时和总超时；
- `RHE-F-005`：按错误类别执行指数退避、抖动、限次重试和 `Retry-After`；
- `RHE-F-006`：Provider Broker 根据模型约束、成本预算、健康度、数据策略和显式路由做选择与故障转移；
- `RHE-F-007`：支持幂等键、请求摘要、响应完整性、usage/cost 对账和 outcome unknown；
- `RHE-F-008`：凭据只使用 Cista 的短时 Secret binding。

### 9.2 `model.call.v1`

请求至少包含：model selector、ModelSurface hash、message/tool schema refs、token budget、采样参数、stream 选项、provider policy、idempotency key 和 deadline。

```text
model.requested → model.dispatched
→ model.reasoning-chunk / model.content-chunk / model.tool-call-delta
→ model.tool-call and/or assistant.message
→ model.usage → model.completed
```

流式 chunk 可经 Lemon 合批，但最终 `assistant.message`、完整 structured tool call 与 usage 必须可独立重建。

### 9.3 故障转移规则

- 鉴权、内容策略和错误请求不重试；
- 限流和暂时性服务错误可以在预算内重试；
- 已产生不可重复输出后切换 provider，必须开始新的 attempt，并保留旧 attempt；
- provider 切换不得把不兼容模型伪装成同一模型；
- 可能已被 provider 接收但没有终态的请求记录为 `outcome-unknown`。

### 9.4 验收

- 每个 provider 有录制协议 fixture 和受凭据保护的可选真实集成测试；
- 流中断后最终消息不会拼接重复或乱序 token；
- tool call 增量可还原并通过 schema 校验；
- 重试总次数、token 和费用不超过 budget；
- Photon、日志、UI 与抓包诊断中不存在密钥明文。

## 10. Janus：默认单 Agent 闭环

### 10.1 状态机

```text
created
→ need-model
→ waiting-model
├─ assistant-final → completed
└─ tool-call → waiting-tool → need-model

任意非终态
├─ cancel → cancelled
├─ budget exhausted → budget-exhausted
├─ repeated normalized Act → oscillation-stopped
└─ unrecoverable failure → failed
```

### 10.2 功能职责

- `JAN-F-001`：从 committed Photon 确定性重建 turn/step 状态；
- `JAN-F-002`：在 `need-model` 时提出唯一 `model.call`；
- `JAN-F-003`：关联 model tool call、目标 Act、tool result 与下一轮模型调用；
- `JAN-F-004`：执行 step、token、费用、时间和工具次数预算；
- `JAN-F-005`：检测相同规范化 Act、无进展循环和重复失败；
- `JAN-F-006`：处理 steer、cancel、stop 和自然停机；
- `JAN-F-007`：暴露 ray status、当前 step、等待原因和终止原因。

Janus 不隐式启动 Clotho 工作流或 Aya 子运行；这些能力必须来自明确 Act。

### 10.3 验收

- 普通对话、一次工具调用、多工具调用、工具失败、用户 steer、取消、超预算均有 golden ray；
- 相同 PhotonWindow 与 epoch 产生字节级相同提案；
- 没有提案且没有在途 Beam 时自然追加 `ray.darkened`；
- daemon 重启后从 Photon 恢复到同一状态，不重复执行不可幂等 Act。

## 11. Clotho：确定性 YAML 工作流

### 11.1 工作流格式

工作流定义使用 YAML，并在首次运行时把规范化 definition hash 追加为 `workflow.defined`：

```yaml
api: tokmon.workflow/v1
name: review-and-test
inputs:
  workspace: { type: string }
nodes:
  inspect:
    act: workspace.inspect
    with: { root: "${inputs.workspace}" }
  tests:
    needs: [inspect]
    act: process.exec
    with: { argv: ["ctest", "--output-on-failure"] }
    retry: { max_attempts: 2, backoff_ms: 500 }
  review:
    needs: [inspect]
    child: { task: "review changes", workspace: readonly }
  publish:
    needs: [tests, review]
    when: "steps.tests.status == 'succeeded'"
    act: artifact.create
failure: stop
```

表达式语言必须无 I/O、无动态代码求值、类型受限且结果确定。模板只能读取声明的 inputs、step outputs 和常量。

### 11.2 功能职责

- `CLO-F-001`：解析并校验 DAG、schema、依赖、循环、模板、条件和权限上界；
- `CLO-F-002`：支持顺序节点、并行节点、条件分支、fan-out/fan-in、批处理 stage 和显式 join；
- `CLO-F-003`：确定性选择 ready 节点，并服从全局/分组并发上限；
- `CLO-F-004`：支持 retry、timeout、continue、stop、compensation proposal 等失败策略；
- `CLO-F-005`：支持 pause/resume/cancel 和指定失败节点的补跑；
- `CLO-F-006`：每个 attempt、条件结果、输出引用和 join 结果都形成 Photon；
- `CLO-F-007`：重启后只从 Photon 恢复，不依赖内存调度状态。

### 11.3 验收

- 同一定义、输入和结果事实产生相同 ready 集与排序；
- 无环、循环、缺依赖、模板越权和类型错误均被验证器识别；
- 并行完成顺序不同不改变 join 语义；
- 补跑产生新 attempt，不覆盖旧结果；
- 运行中修改 YAML 不改变已启动实例，只影响新 workflow version。

## 12. Aya：多 Agent 子运行与协同

### 12.1 功能职责

- `AYA-F-001`：支持 `fork`（继承选定历史）和 `spawn`（以任务包启动）两种子运行；
- `AYA-F-002`：为每个 child 建立独立 ray/stream、预算、deadline 和允许的 ActKind 上界；
- `AYA-F-003`：按策略继承 instruction、memory refs、artifact refs 和只读上下文，禁止隐式继承 Secret binding；
- `AYA-F-004`：默认只读共享工作区；需要写入时创建独立 Git worktree；
- `AYA-F-005`：支持 parent/child 和 sibling 消息、进度、请求帮助、取消传播与心跳；
- `AYA-F-006`：支持 all/any/quorum/manual join，并生成摘要、artifact refs 和冲突清单；
- `AYA-F-007`：文件合并只提出 Cove merge Act，绝不直接覆盖父工作区；
- `AYA-F-008`：支持对子运行进行成本、token、时间和并发配额统计。

### 12.2 Act 与状态

| Act | 关键参数 | 结果 |
| --- | --- | --- |
| `child.spawn.v1` | task、context policy、budget、workspace mode、join policy | child started/failed |
| `child.message.v1` | sender、recipient、payload/ref | message delivered/rejected |
| `child.join.v1` | child ids、policy、deadline | joined/partial/timeout |
| `child.cancel.v1` | propagation、reason | cancel requested/observed |
| `workspace.merge-proposal.v1` | worktree、base、target | 交给 Cove/Fallen |

### 12.3 验收

- 子预算、权限和 secret 范围不能超过父级授权上界；
- child 崩溃不会破坏 parent stream；
- worktree 写入彼此隔离，合并冲突不会自动覆盖；
- join 只向父流追加新摘要/引用；
- daemon 重启后恢复 child 拓扑和未完成 join。

## 13. Textus：ModelSurface 与上下文预算

### 13.1 功能职责

- `TEX-F-001`：按确定顺序组装 system fragments、instruction、conversation、tool schemas、tool results、memory 和 RAG 片段；
- `TEX-F-002`：使用目标模型 tokenizer 或经过校准的保守 estimator 计算 token；
- `TEX-F-003`：为不同来源设置最小保留、最大占比、优先级、信任和敏感级别；
- `TEX-F-004`：始终保留最新用户输入、未完成 tool call 及其依赖；
- `TEX-F-005`：执行去重、相关性排序、滑动窗口、摘要、截断和 Chora 溢出；
- `TEX-F-006`：显式记录每个片段的保留/删除/压缩原因和 source hash；
- `TEX-F-007`：将 reasoning/content/tool-result 分别投影，避免隐藏内容误入普通消息；
- `TEX-F-008`：缓存键包含 tail seq、epoch、model id、tokenizer 和 reducer version。

### 13.2 预算顺序

```text
安全与根指令
→ 当前用户输入
→ 未完成工具调用及结果
→ 当前任务显式 instruction/skill
→ 最近对话
→ 已接受 memory
→ RAG 片段
→ 历史摘要
```

优先级不是信任等级；低信任外部文本即使高度相关，也只能作为 data。

### 13.3 摘要契约

`text.summarize.v1` 产生 `summary.created`，包含 covered range、covered hash、summary model、prompt version、token counts、source refs 和质量标记。摘要不能删除源 Photon，也不能把推测写成已确认事实。

### 13.4 验收

- 相同输入和配置产生字节级稳定 ModelSurface；
- 每个目标模型的最终 token 不超过硬窗口和请求预算；
- 已拔出工具的历史调用保留为叙述，但不进入当前 tool schema；
- 超限时 UI/诊断明确显示压缩、截断和溢出原因；
- Prompt 本身不是规范事实源，可从 Photon 重建。

## 14. Enso：技能、长期记忆与 RAG

### 14.1 SKILL.md 渐进加载

- `ENS-F-001`：发现用户级、项目级和已装镜 artifact 中的 SKILL.md；
- `ENS-F-002`：先读取元数据和触发条件，只在匹配任务时加载正文；
- `ENS-F-003`：解析引用资源时限制在允许根，并记录内容 hash 和加载链；
- `ENS-F-004`：skill 更新通过新 artifact/config observation 生效，缓存按 hash 失效；
- `ENS-F-005`：skill 内容进入 ModelSurface 时标明来源、版本、信任与权限要求。

### 14.2 长期记忆

- `ENS-F-006`：从明确事实中提出偏好、项目约定、经验和实体关系候选；
- `ENS-F-007`：memory proposal 经 Fallen 策略和必要的人类确认后成为 accepted/rejected Photon；
- `ENS-F-008`：支持 scope、来源、置信度、有效期、敏感级别和 supersedes 关系；
- `ENS-F-009`：所谓“更新记忆”追加新版本或失效事实，不改写旧 memory Photon。

### 14.3 RAG

- `ENS-F-010`：从 Cove 文件、Chora artifact 和允许的外部数据源摄取文档；
- `ENS-F-011`：完成解析、chunk、Embedding、向量/关键词混合索引、过滤和重排；
- `ENS-F-012`：结果必须带 document/chunk/hash/path/revision/score/source；
- `ENS-F-013`：索引是可重建派生物，原文与摄取事实才是依据；
- `ENS-F-014`：文件 watcher 触发增量重建，删除文件追加 tombstone observation。

### 14.4 验收

- 可以加载真实 SKILL.md 及其受限引用资源；
- prompt injection 文本不会被提升为 instruction；
- memory 不会在无策略情况下自动永久保存；
- 修改一个文件只重建受影响 chunk，查询不会返回过期 revision；
- 拔出 Enso 后，下一拍不再贡献 skill/memory/RAG 上下文。

## 15. Techor：工具目录、Schema 与 Code Mode

### 15.1 功能职责

- `TEC-F-001`：建立统一工具目录，来源包括当前 LightPath、Iris MCP、内置 C++、Node.js/CPython worker 和显式 Code Mode；
- `TEC-F-002`：为每项工具记录 name、description、input/output schema、ActKind、target、generation、epoch、risk 和来源；
- `TEC-F-003`：把允许且适合当前模型的工具投影到 `model.tools`；
- `TEC-F-004`：只接受 structured tool call，执行严格 schema 校验、默认值填充、规范化和唯一目标匹配；
- `TEC-F-005`：拒绝 unknown tool、旧 epoch、schema drift、重复路由和参数混淆；
- `TEC-F-006`：Code Mode 将代码解析/编译为受限结构化 Act 序列，不能直接获得 I/O；
- `TEC-F-007`：大工具结果存 Chora artifact，Photon 和 ModelSurface 使用预算化摘要；
- `TEC-F-008`：工具动态增删在下一次 Surface fold 生效，历史调用仍可阅读。

### 15.2 Code Mode

Code Mode 必须声明语言、源 hash、允许 API、最大 Act 数、预算和目标 sandbox。代码先经解析和策略检查，再由 Styx 执行；Host API 只允许提出 Act、读取显式输入和写临时结果。`eval`、shell 字符串和直接 daemon 对象访问不能作为安全边界。

### 15.3 验收

- 本地工具与 MCP 工具通过同一 tool-call fixture；
- JSON Schema 边界值、`oneOf`、`required`、`additionalProperties` 和数值范围得到严格处理；
- `(ActKind, schema)` 在一个 epoch 中只能命中一个 target generation；
- Code Mode 产生的文件、网络和进程操作仍经过 Fallen/Cista/Styx；
- 工具换代后旧 schema call 被拒绝并要求模型重新规划。

## 16. Fallen：策略、审批与内容安全

### 16.1 功能职责

- `FAL-F-001`：按 deny → allow → ask 顺序计算准入，policy 解析失败默认 deny；
- `FAL-F-002`：根策略限定项目配置可请求的最大文件、网络、进程、secret 和安装权限；
- `FAL-F-003`：按 ImpactClass、trust、workspace、target、参数和时间窗口匹配策略；
- `FAL-F-004`：支持单级或多级审批瀑布、超时、一次性/会话级决定和最小权限差异；
- `FAL-F-005`：审批绑定 Act hash、epoch、generation 和 deadline，任何变化使批准失效；
- `FAL-F-006`：对模型输入输出、外部文档和工具结果执行内容策略分类；
- `FAL-F-007`：所有目标 Lens 之前必须经过同一 admission，UI/CLI/Worker 都不能绕过。

### 16.2 策略 YAML

```yaml
fallen:
  defaults: deny
  rules:
    - effect: allow
      acts: [fs.read]
      paths: ["${workspace}/**"]
    - effect: ask
      acts: [fs.write, git.commit]
    - effect: deny
      acts: [process.exec]
      argv0: ["powershell", "cmd"]
  approvals:
    external_irreversible: [user]
```

### 16.3 验收

- deny 规则不能被更低优先级 allow 覆盖；
- 项目 `.tokmon` 不能扩大用户根策略或增加信任根；
- 审批画面精确显示路径、命令、网络目标、secret purpose、Diff 和风险；
- 修改任一 Act 参数后旧批准无效；
- 未批准的 external irreversible Act 永远无法获得目标 Beam。

## 17. Cista：操作系统凭据库与脱敏

### 17.1 功能职责

- `CIS-F-001`：Windows 使用 Credential Manager/受保护存储，macOS 使用 Keychain，Linux 使用 Secret Service 或明确配置的安全后端；
- `CIS-F-002`：YAML、Photon 和 Surface 只保存 `SecretRef`，不保存密钥明文；
- `CIS-F-003`：支持 create/read/rotate/delete/list-metadata，所有变更追加审计 Photon；
- `CIS-F-004`：为 exact Act、target generation、purpose 和 deadline 生成一次性短时 binding；
- `CIS-F-005`：只在最终网络/进程边界把 binding 解析为 header、环境变量或请求字段；
- `CIS-F-006`：对日志、URL、header、正文、异常、artifact、遥测和诊断包执行 schema-aware redaction；
- `CIS-F-007`：维护 secret 指纹匹配器，但不把可逆密钥材料写入索引。

### 17.2 SecretRef

```yaml
secret_ref:
  provider: os-keyring
  id: model/openai/default
  purpose: model-api
```

Surface 只能显示 provider、id、purpose、availability、last_rotated 和 policy；不可显示 value、可逆编码或完整指纹。

### 17.3 验收

- 扫描数据库、日志、UI snapshot、Blob metadata、crash dump 和遥测 fixture，均无测试 secret 明文；
- binding 过期、Act hash 变化或 generation 换代后立即失效；
- redaction 无法确认安全时阻止外发或诊断包生成；
- OS 安全后端不可用时明确降级为 unavailable，不能偷偷写入 YAML/普通文件。

## 18. Styx：本地与远程隔离执行

### 18.1 功能职责

- `STY-F-001`：把已准入 Act 转换为包含 argv、cwd、env allowlist、文件/网络范围、CPU/Mem/PID/output/deadline 的 `SandboxPlan`；
- `STY-F-002`：在各平台使用可验证的 OS 隔离后端，并报告实际 `SandboxStrength`；
- `STY-F-003`：支持本地进程、Node.js/CPython worker、WASM、PTY 会话和远程容器后端；
- `STY-F-004`：远程容器支持创建、上传输入、执行、流式输出、下载 artifact、取消和销毁；
- `STY-F-005`：PTY 支持 resize、stdin、stdout/stderr、退出状态和空闲超时；
- `STY-F-006`：输出使用有界 ring，洪泛时截断并产生明确 Photon；
- `STY-F-007`：取消先 cooperative，再终止整棵进程树；
- `STY-F-008`：Secret 只在 Cista 批准的最终边界注入，执行后清理临时材料。

### 18.2 安全规则

- shell 文本不能替代 argv；需要 shell 时必须显式选择 shell、单独审批并记录；
- cwd 和挂载路径必须由 Cove canonicalize；
- 无法提供声明强度时拒绝执行，不能静默降级；
- 网络默认关闭，域名/IP/端口/协议分别受限；
- E2B 等远程后端是可替换 adapter，后端身份和区域必须进入执行事实。

### 18.3 Act 与结果

| Act | 关键参数 | 结果 Photon |
| --- | --- | --- |
| `process.exec.v1` | argv、cwd、env refs、limits、deadline | process completed/failed/timed-out |
| `process.cancel.v1` | process/beam id、grace period | cancel requested/cancelled/completed-after-cancel |
| `pty.open/write/resize/close.v1` | terminal profile、dimensions、input | pty opened/chunk/resized/closed |
| `worker.launch.v1` | runtime ref、artifact、protocol | worker ready/crashed/stopped |
| `wasm.invoke.v1` | module hash、export、capabilities | wasm completed/failed |
| `remote.execute.v1` | backend、region、image、inputs | remote completed/failed/outcome-unknown |

### 18.4 验收

- 测试逃逸路径、符号链接、fork bomb、输出洪泛、超时、取消和进程树残留；
- PTY 真实交互、resize 和 UTF-8/CJK 输出正确；
- 网络 allowlist 与文件只读/可写范围实际生效；
- 后端不可用时返回结构化错误，不在宿主裸执行；
- afterglow 完成后该 generation 的进程、PTY、容器和临时目录归零。

## 19. Chora：会话、KV、Blob 与不可改写存储

### 19.1 功能职责

- `CHO-F-001`：SQLite WAL 持久化 PhotonEnvelope、stream、ray、schema 和 mount epoch；
- `CHO-F-002`：单 writer 事务分配 seq、补 parent、canonical encode、计算 hash 并 durable commit；
- `CHO-F-003`：提供版本化 KV；每次 put/delete 都追加新版本，current view 由版本折叠得到；
- `CHO-F-004`：提供内容寻址 Blob，支持流式写入、去重、引用计数显像和完整性校验；
- `CHO-F-005`：敏感 Blob 使用 envelope encryption，数据密钥由 Cista 管理；
- `CHO-F-006`：生成可重建 checkpoint、不可变 archive、压缩和保留策略；
- `CHO-F-007`：备份/恢复包含 DB、WAL 状态、Blob、schema bundle、mount epoch 与校验清单；
- `CHO-F-008`：容量不足、磁盘错误和损坏时进入明确只读/降级状态。

### 19.2 存储边界

Photon 是事实；KV current table、全文索引、向量索引和 checkpoint 是派生视图。Blob 内容不可原地覆盖；同一逻辑文件的新内容产生新 hash。保留策略只能清理由策略允许且已封存的派生数据，不能破坏声明保留的因果记录。

### 19.3 Act 与结果

| Act | 结果 |
| --- | --- |
| `photon.export.v1` | 带范围、schema 和完整性清单的 export artifact |
| `kv.put/delete.v1` | 新 KV version/tombstone Photon |
| `blob.put.v1` | 内容 hash、大小、加密与存储位置元数据 |
| `checkpoint.build.v1` | 可验证、可删除重建的 checkpoint |
| `archive.seal.v1` | 不可变 archive 和 manifest |
| `backup.create/restore.v1` | backup/restore 验证结果 |

### 19.4 验收

- SQLite trigger 拒绝 committed Photon 的 update/delete；
- crash matrix 下无半条 Photon、双 writer、重复 seq 或错误 hash；
- KV 历史版本可追溯，删除表现为新 tombstone；
- Blob bit rot 可以被 hash 校验发现；
- 备份恢复后 tail、schema、mount epoch、Blob 引用和因果链一致。

## 20. Tracket：因果轨迹、Vault 与回放

### 20.1 功能职责

- `TRA-F-001`：验证 seq、id、stream、ray、parent、epoch、schema、payload hash、previous hash 和 caused-by-act；
- `TRA-F-002`：构建 timeline、因果 DAG、Act proposal/admission/execution/result 链和跨 ray 引用；
- `TRA-F-003`：把原始 provider frame、worker frame、终端 chunk 等写入压缩 Raw Trace Vault；
- `TRA-F-004`：提供 R0 Transcript、R1 Surface、R2 Control 和 R3 Live 四级回放；
- `TRA-F-005`：创建 ray fork、trajectory export、完整性报告和审计包；
- `TRA-F-006`：未知 required schema、断链和非法 parent 阻止可信回放并产生诊断；
- `TRA-F-007`：导出前经过 Fallen 和 Cista，支持范围、敏感级别和目的绑定。

### 20.2 回放级别

| 级别 | 行为 | 现实动作 |
| --- | --- | --- |
| R0 | 从 Photon 重建对话、工具、Diff 和状态 | 否 |
| R1 | 使用指定 LightPath 重建 Surface | 否 |
| R2 | 使用记录的模型/工具结果重演控制决定 | 否 |
| R3 | 在新 fork 重新调用模型和工具 | 是，必须重新准入 |

R3 永远创建新 stream；任何级别都不能向源 stream 插入、替换或删除 Photon。

### 20.3 验收

- 篡改 payload、parent、schema 或 hash 后验证必然失败；
- R0/R1/R2 不产生现实 I/O；
- golden ray 忽略时间和随机 id 后语义一致；
- Vault 压缩/解压后 frame 顺序、校验值和引用一致；
- 导出物包含 schema 和完整性清单，且不泄露 Secret。

## 21. Nota：日志、OpenTelemetry 与实时诊断

### 21.1 功能职责

- `NOT-F-001`：统一接收 spdlog 结构化事件，并附加 ray/step/photon/act/beam/lens/generation/epoch correlation；
- `NOT-F-002`：生成 engine、model、tool、workflow、worker、DB、Snow 和 UI 的 OpenTelemetry Span；
- `NOT-F-003`：聚合 latency、throughput、errors、queue、bytes、token、cost、cache 和 sandbox 指标；
- `NOT-F-004`：支持 OTLP、Jaeger/Zipkin 兼容 trace 路径和 Prometheus metrics endpoint；
- `NOT-F-005`：运行时调整日志级别、采样率和过滤器，配置变化追加 observation Photon；
- `NOT-F-006`：提供 health、doctor、profile capture、诊断包和 crash summary；
- `NOT-F-007`：所有队列有界，exporter 失败或遥测删除不影响事实提交、恢复和回放；
- `NOT-F-008`：在进入任何 sink 前调用 Cista 脱敏。

### 21.2 最小指标

- engine step/view/refraction 的 p50/p95/p99；
- Photon append latency、WAL checkpoint、DB/Blob 容量；
- 每个 conduit 的 queue、lag、drop/coalesce；
- provider 首 token/总延迟、重试、token、费用；
- worker heartbeat、restart、memory、CPU；
- LightPath reconcile、dark lane、R1/R2 handoff 和 afterglow；
- UI delta backlog、frame update 和 reconnect。

### 21.3 Act 与验收

`telemetry.export.v1`、`profile.capture.v1`、`diagnostic.bundle.v1` 分别产生导出、profile 和诊断包结果；profile 与诊断包必须先经过 Fallen/Cista。

- trace context 跨 C++、Node.js、CPython、MCP 和模型调用传播；
- exporter 离线时有界缓存且不阻塞 append writer；
- 动态改日志级别不需要重启，但变化可审计；
- Span 默认不记录 prompt、文件正文、tool payload 或 secret；
- 诊断包经审批、脱敏并附带内容清单。

## 22. Cove：工作区、文件监听、Git 与 Artifact

### 22.1 功能职责

- `COV-F-001`：维护 workspace entity tree，包括 canonical path、类型、大小、mtime、content hash、Git 状态和忽略状态；
- `COV-F-002`：跨平台监听 create/modify/delete/rename，执行 debounce、批量聚合、溢出检测和必要的全量 rescan；
- `COV-F-003`：捕获分支、HEAD、index、worktree、untracked 和 conflict 的 Git 快照；
- `COV-F-004`：文件写入前保存 preimage hash/Blob ref，写后重读形成 postimage hash/Blob ref 与实际 Diff；
- `COV-F-005`：执行 read/write/create/move/delete，并使用 precondition hash 防止并发覆盖；
- `COV-F-006`：执行 Git stage/commit/branch/merge/rebase proposal，并保留 stdout/stderr/exit evidence；
- `COV-F-007`：创建内容寻址 artifact、preview 和 provenance；
- `COV-F-008`：把文件观察贡献给 Textus/Enso，但不会把变更事件直接当作 instruction。

### 22.2 路径安全

每次现实操作都必须：解析 workspace root → canonicalize 目标 → 逐段检查 symlink/reparse point → 验证根内约束 → 验证 precondition → 执行 → 重新打开并读取 → 计算结果 hash。检查与执行应尽量使用安全句柄，避免 TOCTOU。

### 22.3 文件监听

Watcher event 只是“需要重新观察”的提示。Cove 必须重新读取实际文件状态后才追加 `fs.observed/changed/deleted`。队列溢出时追加 `watcher.overflowed` 并触发 rescan，不能假装事件完整。

### 22.4 Act

| Act family | 能力 |
| --- | --- |
| `fs.read/write/create/move/delete.v1` | 文件读取与受前置条件保护的变更 |
| `git.status/stage/commit/branch/merge/rebase.v1` | Git 观察与动作 |
| `workspace.scan/watch.v1` | 实体树全量扫描与 watcher 生命周期 |
| `artifact.create/preview/export.v1` | 内容寻址 artifact 与显像 |

### 22.5 验收

- traversal、根外路径、符号链接逃逸、reparse point 和参数混淆被拒绝；
- 并发修改触发 precondition conflict；
- watcher 重复/乱序/丢事件后实体树可通过 rescan 收敛；
- Diff 来自写后重读，不来自模型的预期文本；
- Git 合并冲突不会自动覆盖，并形成可供 Termon/Snow 显示的冲突 artifact。

## 23. Snow：CLI、stdio 与本地协议

### 23.1 功能职责

- `SNO-F-001`：提供交互式 CLI、单次 headless `run`、会话 `chat` 和历史查询；
- `SNO-F-002`：提供 machine-readable JSON Lines/CBOR 输出，禁止混入 human decoration；
- `SNO-F-003`：提供 stdio server，支持 request id、并发请求、stream event、cancel、错误和有序关闭；
- `SNO-F-004`：提供 Windows named pipe / Unix domain socket 本地协议，服务 Termon 和其他本地客户端；
- `SNO-F-005`：支持 snapshot + cursor delta、gap 检测、重连和幂等 request id；
- `SNO-F-006`：提供 doctor，检查配置、数据库、LightPath、artifact、runtime、provider、sandbox、secret backend 和 UI 连接；
- `SNO-F-007`：作为 CI/CD 脚本入口，提供稳定退出码、deadline、无颜色模式和输出文件；
- `SNO-F-008`：提供 lens list/verify/mount/replace/unmount/reconcile 等组合管理命令。

### 23.2 协议约束

frame 包含 magic、major/minor、flags、payload length、request id、cursor 和 canonical CBOR payload。超大帧、半帧、非 canonical payload 和 major mismatch 必须拒绝。同一用户身份是默认连接边界。

### 23.3 CLI 命令面

```text
tokmon chat
tokmon run --prompt <text> [--output human|jsonl|cbor]
tokmon serve --stdio
tokmon history <ray>
tokmon cancel <ray-or-act>
tokmon doctor [--json]
tokmon lens list|verify|mount|replace|unmount|reconcile
tokmon config paths|validate|show
```

Snow 只负责输入折射和 CliSurface 显像，不拥有 Agent 状态，也不是 Photon writer。

### 23.4 验收

- human、JSON Lines、CBOR 和 stdio 输出彼此隔离；
- 管道关闭、客户端崩溃和重连不影响 daemon ray；
- cursor gap 必须先 snapshot 后继续 delta；
- doctor 每项给出 evidence、严重级别和可执行修复建议；
- CI 模式无交互审批时按策略失败，而不是无限等待。

## 24. Termon：Slint Workbench

### 24.1 功能页面

- `TER-F-001` 会话：新建/切换会话、流式回答、消息状态、停止、重试和 artifact；
- `TER-F-002` 轨迹：Agent step、模型、工具、审批、文件、子运行和因果关系时间线；
- `TER-F-003` 上下文检查器：ModelSurface 来源、token 占比、裁剪/摘要原因、trust/sensitivity；
- `TER-F-004` 模型：provider/model 选择、窗口、价格、限额、健康度和 usage；
- `TER-F-005` 工具与执行：tool call、参数、结果、终端流、SandboxStrength 和资源用量；
- `TER-F-006` 工作区：文件树、Diff、Git 状态、artifact preview 和冲突处理；
- `TER-F-007` 审批：最小权限差异、风险、Diff、命令、网络目标、secret purpose 与允许/拒绝；
- `TER-F-008` 子运行：拓扑、进度、预算、worktree、join 和 merge proposal；
- `TER-F-009` 透镜：desired/current LightPath、版本、generation、权限、验证、换代和 afterglow；
- `TER-F-010` 诊断：日志、Span、指标、队列、数据库、worker、doctor 和诊断包；
- `TER-F-011` 设置：编辑用户级/项目级 YAML 的受控表单和原始模式，保存前 schema 校验。

### 24.2 Slint 边界

- Slint event loop 固定在 desktop 主线程；
- Snow I/O 位于受管 worker，约 16 ms 批量投递不可变 projection model；
- timeline、terminal、Diff 和日志使用虚拟化/有界模型；
- UI intent 通过 Snow 发给 daemon，Termon 不能直接写 Photon、文件、配置或密钥；
- UI 关闭、断线或崩溃不改变 daemon 中的 ray；重启后按 cursor 重建；
- Termon R2 换代由 launcher 启动候选 desktop、同步 cursor、切换入口并关闭旧进程。

### 24.3 视觉与可用性验收

- 严格实现指定 Figma 的布局、组件、状态、间距、颜色和交互；
- CJK、IME、键盘导航、焦点、屏幕阅读器、200% DPI 和主题可用；
- 10k trajectory、持续 token stream 和大 Diff 下仍可交互；
- 所有 loading/empty/error/offline/reconnecting/degraded/approval 状态闭合；
- 页面显示的数据都能追溯到 UiSurface/DiagnosticSurface，不保留隐藏业务状态。

## 25. 端到端功能闭环

### 25.1 用户请求修改代码

```text
Termon/Snow 提交 user intent
→ Textus + Enso 组装 ModelSurface
→ Janus 提出 model.call
→ Rhea 流式产生 model.tool-call
→ Techor 解码 fs.write
→ Fallen 展示权限、路径和 Diff 并审批
→ Cista 绑定必要 Secret/制定脱敏计划
→ Styx 建立执行边界
→ Cove 验证 preimage、写入、重读并生成 postimage/Diff
→ Nota 记录 Span/Metrics
→ Tracket 验证因果链
→ Chora 提交所有新 Photon/Blob
→ Termon/Snow 显示已观察到的结果
```

### 25.2 MCP 工具调用

```text
Iris 发现远端 MCP tool + schema hash
→ Techor 投影为当前 model.tools
→ Rhea 产生 structured tool call
→ Techor 校验并规范化 external.call
→ Fallen/Cista 准入
→ Iris 调用远端并区分完成/失败/超时/结果未知
→ 大响应进入 Chora Blob，摘要进入 Textus
```

### 25.3 子运行与 Worktree 合并

```text
Janus/Clotho 提出 child.spawn
→ Fallen 限定预算、ActKind 与 workspace mode
→ Aya 创建 child stream + Cove worktree
→ child 独立运行并持续报告进度
→ Aya join 产生 summary/artifact/conflict refs
→ Cove 提出 merge Diff
→ Fallen 再次审批
→ 合并结果作为新 Photon 进入父 ray
```

### 25.4 运行时换镜

```text
.tokmon/light-path.yaml changed
→ config.light-path-observed
→ Ignis resolve/verify/dark-lane
→ permission delta admission
→ mount epoch committed
→ Nyxia atomic publish E+1
→ new Beam only routes to new generations
→ old generations drain and stop
```

### 25.5 崩溃恢复

```text
open SQLite WAL → validate Photon tail/hash
→ restore last complete mount epoch
→ reconstruct LightPath and active rays
→ mark unterminated non-idempotent Acts outcome-unknown
→ reconcile safe idempotent Acts or ask user
→ Snow/Termon reconnect by cursor
```

## 26. 配置与 Manifest 最低要求

### 26.1 `lens.yaml`

每个正式透镜必须声明：

```yaml
api: tokmon.lens/v1
id: org.tokmon.lens.example
version: 1.0.0
runtime:
  kind: native-cabi       # native-cabi | native-worker | node | cpython | wasm | desktop
  abi: tokmon-lens-c-v1
  entry: payload/example
observes: []
surfaces: []
acts: []
permissions:
  filesystem: []
  network: []
  secrets: []
resources:
  memory_mb: 256
  output_bytes: 1048576
  deadline_ms: 30000
replacement: R1
```

manifest 必须与 artifact hash、schema bundle、runtime hash、依赖 lock、SBOM 和签名绑定。Node.js/CPython artifact 不得从系统 PATH、全局包目录或联网 registry 浮动解析依赖。

### 26.2 配置所有权

| 配置段 | 所有者 |
| --- | --- |
| `nyxia`、`storage`、`workers` | Nyxia/Chora |
| `light_path`、`artifacts`、`trust` | Ignis |
| `models`、`providers`、`routing` | Rhea |
| `mcp`、`lsp`、`external` | Iris |
| `policy`、`approvals` | Fallen |
| `secrets` metadata | Cista；明文不在 YAML |
| `sandbox`、`remote_execution` | Styx |
| `workspace`、`git`、`watcher` | Cove |
| `memory`、`skills`、`rag` | Enso |
| `telemetry`、`logging` | Nota |
| `cli`、`protocol`、`ui` | Snow/Termon |

## 27. 测试与完成定义

### 27.1 每镜契约测试

十九个正式透镜都必须通过：manifest/YAML 等价、确定性 view、声明 SurfaceChannel、ActPattern、错误 schema、deadline/cancel、拔镜零新贡献、dark lane、换代、崩溃边界和因果输出测试。

### 27.2 真实能力测试

以下能力不能只用“生成了 requested Photon”作为完成证明：

- Iris：必须启动真实 MCP/LSP fixture 并完成协议往返；
- Rhea：必须完成真实 provider 或官方兼容 mock server 的 streaming/retry/failover；
- Aya：必须真的创建 child stream 和隔离 worktree；
- Enso：必须解析真实 SKILL.md、建立索引并检索命中文档；
- Techor：必须动态发现至少一个 MCP 工具并完成 schema 调用；
- Styx：必须用逃逸/配额/PTY fixture 证明实际隔离；
- Cista：必须读写平台安全存储并完成全出口明文扫描；
- Nota：必须由测试 collector 接收到真实 OTel Span/Metrics；
- Cove：必须由真实 watcher/Git 仓库验证收敛与前后镜像；
- Snow：必须通过真实 stdio、pipe/socket 和断线重连；
- Termon：必须连接真实 projection，不以硬编码示例数据完成验收。

### 27.3 系统完成门槛

只有同时满足以下条件，才能声明“内置透镜完整实现”：

1. 本文所有 `*-F-*` 功能存在实现与自动化测试；
2. 十九镜可分别被装入、换代、拔出，且拔出后零新贡献；
3. 固定 Act 管线不可绕过，Secret 明文扫描为零；
4. MCP、LSP、真实模型、子运行、RAG、强沙箱、OS 凭据库、OTel、watcher、stdio 和 Slint 实时数据全部完成端到端验证；
5. Photon 数据库物理只追加，取消、补偿、回放和回滚均不改写历史；
6. Windows、macOS、Linux 的平台能力或明确的受控降级均通过测试；
7. `IMPLEMENTATION-REPORT.md` 为每个功能编号给出源文件、测试、运行证据和未完成项，不使用目录或类型存在代替功能证据。

### 27.4 组合能力测试

- 以属性测试随机生成合法/非法 LightPath，验证路由唯一性、权限单调收紧和原子发布；
- 为每项复合能力至少准备两组等价组合，例如本地工具与 MCP 工具、C++ RAG 与 CPython RAG；
- 在运行中的模型流、PTY、子运行、watcher 和 OTel exporter 存在时执行换镜，验证有界 afterglow；
- 同一 ray 在显式换镜前后分别绑定其当拍 snapshot，不出现混合 epoch；
- 删除任一非必需透镜后系统可继续运行并显像明确降级，重新装入后从 Photon 收敛。

## 28. 代码布局

```text
tokmon-n/
├─ nyxia/                         # 唯一静态微内核
├─ lenses/
│  ├─ ignis/ ... termon/         # 十九个正式透镜
│  ├─ calculator/                 # SDK 参考透镜
│  └─ common/                     # 机械 helper 与 registry，无按名称业务分支
├─ apps/
│  ├─ tokmond/
│  ├─ tokmon-cli/
│  ├─ tokmon-desktop/
│  └─ tokmon-lens-worker/
├─ sdk/
│  ├─ cpp/
│  ├─ node/
│  └─ python/
├─ schemas/
├─ ui/
├─ tests/
└─ docs/
```

每个 `lenses/<name>/` 至少包含独立 C++ 类型、manifest、schema、adapter、配置说明、contract tests 和真实能力 tests。`lenses/common` 只能保存通用校验与 emit helper，禁止根据透镜名字分支实现业务。

## 29. 最终约束

Nyxia 只守住运行规则，不吞并业务；十九个正式透镜可以按用户、项目、任务和 ray 随时组合，并通过候选验证、原子发布和有界余辉安全换代。新能力只需声明自己观察什么、显像什么、能够折射什么，就能进入同一光路；已有能力不必认识它，也不必修改微内核。

Textus/Enso 负责让模型看见正确上下文，Rhea/Janus 负责推演，Techor/Fallen/Cista/Styx 负责让行动经过统一而不可绕过的边界，Cove/Iris 触碰现实，Chora/Tracket 保存不可改写的因果光痕，Nota 让运行可诊断，Snow/Termon 把同一事实显像给命令行和人类。

透镜可以装入、组合、重排、换代和拔出，Surface 可以重建，Act 可以被拒绝或由新的 Act 补偿；已经 committed 的因果光子永不被编辑、修改或撤销。
