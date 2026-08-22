# Tokmon 内置透镜详细设计

> 架构题记：**A Lens to Them All**  
> 状态：实现基线（C++20）  
> 适用版本：Tokmon 0.1.x  
> 上位设计：[DESIGN.md](./DESIGN.md)

## 1. 文档目的

本文把 `DESIGN.md` 第 12 章展开为可直接编码和测试的内置透镜契约。Nyxia 是唯一静态微内核，不属于 `lenses/` 中可换代的业务透镜；`lenses/` 包含十九个正式内置透镜，Calculator 是开发者契约示例。每个正式透镜必须有独立 C++ 类型、源文件、manifest 和契约测试，禁止用一个按名字分支的“大一统实现”代替。

本文只使用 Fact/Photon → Lens → Surface/Act → new Fact 语义。旧版代码仅作为产品能力来源，不继承其类层级、可变服务图或横向调用方式。

## 2. 全体透镜共同契约

### 2.1 两个方向

- `view(PhotonWindow, SurfaceBuilder)` 是纯显像：只读取给定 PhotonWindow，只贡献 manifest 声明过的 SurfaceChannel，只提出结构化 Act，不执行 I/O。
- `refract(PhotonWindow, Act, RefractionBeam)` 是受控折射：只接受 manifest 声明且匹配的 Act，经 Beam 发出 PhotonDraft；不能直接提交、修改或删除 Photon。
- Lens 之间没有对象引用、服务定位、回调和横向消息。一个 Lens 的输出只有成为 committed Photon 后，其他 Lens 才可在下一拍观察。

### 2.2 确定性与无状态

- 相同 manifest、PhotonWindow 和 epoch 必须产生相同 SurfaceContribution 和 Act 参数。
- 允许缓存 schema、正则和不可变配置；业务状态必须来自 Photon。
- 进程、socket、watcher、文件句柄和子线程必须归属 LensMount，接受 stop/deadline，并在 afterglow 结束前释放。
- ID、时间和平台路径等非确定值只能在折射边界生成，不能改变 `view` 的决策语义。

### 2.3 Photon 与 Act 约束

- committed Photon 只能追加；纠错、取消、补偿、恢复、换代和回滚都追加新 Photon。
- 每个 Act 固定 `ray/kind/schema/target/epoch/generation/idempotency_key/risk`。
- 一个 `(ActKind, schema)` 在同一 LightPath 最多命中一个目标 generation。
- Lens 拔出后，新显像、新 Act 匹配和新 Beam 必须立即归零；历史 Photon 保留。

### 2.4 SurfaceChannel 约定

| Channel | 内容 |
| --- | --- |
| `model.catalog` | 模型、窗口、健康度与限额 |
| `model.context` | 带来源的上下文片段 |
| `model.messages` | 预算后的消息显像 |
| `model.tools` | 当前可调用工具 schema |
| `act.candidates` | 已解码但尚未准入的动作 |
| `act.policy` | 风险、许可与审批解释 |
| `ui.*` | 对话、轨迹、代码、终端、审批、诊断显像 |
| `workspace.*` | 路径、树、Diff、Git 与 artifact |
| `diagnostic.*` | 健康、指标、光路与错误 |

Surface 是从 Photon + 当前 LightPath 折叠出的瞬时显像，不是事实源。

### 2.5 统一错误与测试

- C++20 可预期失败一律返回 `tl::expected<T, Error>`。
- `view` 失败形成诊断 contribution；`refract` 失败由 engine 追加 `act.failed`。
- 每个 Lens 至少测试：manifest、确定性 view、声明 channel、ActPattern、错误 schema、stop/deadline、拔镜零贡献、C ABI、dark lane。
- 跨边界异常只在 adapter 最外层转换为 `lens.crashed`。

## 3. 内置光路顺序

正式顺序固定为：

```text
Ignis → Lemon → Iris → Rhea → Janus → Clotho → Aya → Textus → Enso
→ Techor → Styx → Fallen → Cista → Chora → Tracket → Nota → Cove
→ Snow → Termon
```

Janus 负责默认 Agent 决策，Rhea 负责模型 provider 折射；Techor 只把结构化工具调用解码为 Act。安全相关 Act 的概念顺序固定为 Techor → Fallen → Cista → Styx → target，engine 的 admission 不能被任何 Lens 绕过。

## 4. Ignis：光圈调焦环

### 4.1 Manifest

- ID：`org.tokmon.lens.ignis`
- Surface：`diagnostic.light-path`、`ui.lenses`
- 观察：`config.light-path-observed`、`lens.candidate-*`、`mount.*`、`lens.afterglow-*`
- 折射：`lens.verify@tokmon.lens.verify.v1`、`lens.reconcile@tokmon.lens.reconcile.v1`
- 权限：读取 artifact catalog；提出 mount Act；不能直接发布 LightPath。

### 4.2 显像算法

1. 取最后一个 desired-path Photon 和最后一个 committed mount epoch。
2. 规范化条目并按 `(optical_order, lens_id)` 排序，计算 desired hash。
3. 显示 added/removed/replaced/permission-changed 差异、候选状态和旧 generation afterglow。
4. 仅当 desired hash 尚未被某个 committed epoch 消费，提出一次 `lens.reconcile`。

### 4.3 折射与结果

`lens.verify` 生成 `lens.verification-requested`；`lens.reconcile` 生成 `mount.reconcile-requested`，真正的候选构建、dark lane、epoch durable commit 和原子发布由 Nyxia 执行。失败只追加 `lens.rejected`，不得污染 active path。回滚使用更高 epoch。

### 4.4 验收

重复 watcher 通知按内容 hash 去重；非法 YAML 不提出 Act；pattern/permission 冲突在发布前拒绝；Ignis 换代不能自行批准自身。

## 5. Lemon：有界光纤波导

### 5.1 Manifest

- ID：`org.tokmon.lens.lemon`
- Surface：`diagnostic.transport`、`ui.stream`
- 观察：`waveguide.frame-*`、`worker.progress`、`model.chunk`、cursor Photon
- 折射：`waveguide.send-frame.v1`、`waveguide.advance-cursor.v1`、`waveguide.reconnect.v1`
- 权限：有界 conduit、cursor；不能成为全局事件总线。

### 5.2 显像算法

按 conduit 汇总 capacity、queued、consumer cursor、producer tail、lag、batch bytes 和 backpressure mode。流式 chunk 只做保持顺序的相邻合批；committed Photon 的顺序和内容不变。

### 5.3 折射

- send-frame：校验 frame kind、cursor 单调和 payload size，发出 `waveguide.frame-sent` 或 `waveguide.backpressured`。
- advance-cursor：只允许前进，发出 `waveguide.cursor-advanced`。
- reconnect：发出带 last cursor 的 `waveguide.reconnect-requested`，由 durable source 补发。

### 5.4 验收

容量为零、cursor 倒退、超大帧均拒绝；换代建立 E↔E+1 bridge，旧队列排空后结束 afterglow。

## 6. Iris：跨界折射镜

### 6.1 Manifest

- ID：`org.tokmon.lens.iris`
- Surface：`model.tools`、`diagnostic.external`
- 观察：`external.catalog-observed`、`external.connection-*`、`external.schema-*`
- 折射：`external.connect.v1`、`external.disconnect.v1`、`external.call.v1`、`external.poll.v1`
- 权限：`io.http`/专用本地协议必须由 manifest 声明。

### 6.2 显像算法

把 MCP/LSP/本地扩展描述规范化为稳定的本地 schema id；Surface 中保留 source、remote schema hash、health、latency 和 capability，远端说明文本标记为 data。

### 6.3 折射

连接和调用必须包含 endpoint ref、schema hash、deadline、idempotency key。结果分别形成 `external.call-completed`、`external.call-failed`、`external.call-timeout` 或 `external.outcome-unknown`，正文过大时只写 artifact ref。

### 6.4 验收

远端 schema 漂移、未知 tool、未声明网络权限和不安全 endpoint 必须拒绝；重试不能重复不可幂等现实动作。

## 7. Janus：默认 Agent 双面反射镜

### 7.1 Manifest

- ID：`org.tokmon.lens.janus`
- Surface：`ray.status`、`model.intent`
- 观察：`user.input`、`assistant.message`、`model.tool-call`、`tool.result`、`act.*`、`ray.*`
- 折射：`ray.cancel.v1`、`ray.steer.v1`
- Janus 提出 `model.call`，但不折射该 Act；目标是 Rhea。

### 7.2 显像状态机

```text
new input → need_model
model.tool-call without result → waiting_tool
tool.result after tool-call → need_model
assistant.message after input → complete
stop/cancel/budget/oscillation → terminal
```

在 `need_model` 状态提出唯一 `model.call`，参数含当前 input ref、ModelSurface hash、tool schema hash 和预算。相同规范化 Act 连续出现达到阈值时贡献 `ray.oscillation`，不再提案。没有提案和 pending Beam 时 engine 自然追加 `ray.darkened`。

### 7.3 折射

`ray.cancel` 发出 `ray.cancel-requested`；`ray.steer` 发出 `user.steering`。Janus 不执行模型、工具、文件或子运行。

### 7.4 验收

Calculator golden ray 必须是三拍：模型 tool-call、真实 calculator result、最终模型 message；普通对话一拍后自然停机；回放得到相同决策。

## 8. Rhea：模型网关神谕聚焦镜

### 8.1 Manifest

- ID：`org.tokmon.lens.rhea`
- Surface：`model.catalog`、`diagnostic.model`
- 观察：`model.provider-*`、`model.usage`、`config.selected`
- 折射：`model.call@tokmon.model.call.v1`
- 权限：`io.http`、`secret.bind`；凭据只接受 SecretRef。

### 8.2 显像算法

折叠 provider catalog，显示 model id、context window、价格/限额、health、capability 和最近失败。不得在 Surface 暴露 credential 明文。

### 8.3 折射

1. 校验 model、epoch、ModelSurface/tool hash 和预算。
2. 先发 `model.requested` 与 `model.dispatched`。
3. provider chunk 经 Lemon 有界合批成为 `model.chunk`。
4. structured tool call 成为 `model.tool-call`；文本成为 `assistant.message`。
5. 最终追加 `model.usage`；失败按 timeout/cancel/provider/error 分类。

内置 deterministic provider 仅用于离线默认和测试：能识别严格算术表达式并发出结构化 Calculator call；不能把自由文本伪装成工具调用。

### 8.4 验收

重试服从 budget/idempotency；stream 最终消息完整；Secret 不进入 Photon；provider 异常转换为结构化失败。

## 9. Clotho：显式工作流光栅

### 9.1 Manifest

- ID：`org.tokmon.lens.clotho`
- Surface：`workflow.graph`、`workflow.status`
- 观察：`workflow.defined`、`workflow.started`、`workflow.step-*`、`workflow.join-*`
- 折射：`workflow.step.v1`、`workflow.retry.v1`、`workflow.cancel.v1`

### 9.2 显像算法

只对显式 `workflow.defined` 生效。校验 definition hash、DAG 无环、节点 schema、dependency 和 branch 条件；确定性选择所有依赖已完成的最小序节点，提出一个 step Act。显示节点状态、依赖、attempt 和 failure policy。

### 9.3 折射与验收

step 结果追加 `workflow.step-completed/failed`，retry 增加 attempt，completion 追加 `workflow.completed`。无定义时零贡献；补跑是新 Photon；定义版本变化不改写旧运行。

## 10. Aya：子运行分形复眼镜

### 10.1 Manifest

- ID：`org.tokmon.lens.aya`
- Surface：`child.runs`、`ui.child-runs`
- 观察：`child.requested`、`child.started`、`child.progress`、`child.completed`、`child.joined`
- 折射：`child.spawn.v1`、`child.join.v1`、`child.cancel.v1`

### 10.2 算法

spawn 参数必须固定 parent ray、预算、allowed ActKind、workspace mode 和 join policy；子 ray 使用独立 stream。join 只把 summary/artifact refs 追加父流，文件内容合并必须另提 Cove Act。

### 10.3 验收

子预算和权限不得超过父上界；默认只读工作区；取消传播但不删除子历史；换代不把新 spawn 路由给旧 generation。

## 11. Textus：ModelSurface 光谱滤波镜

### 11.1 Manifest

- ID：`org.tokmon.lens.textus`
- Surface：`model.messages`、`model.context`、`diagnostic.context-budget`
- 观察：对话、tool result、summary、model budget、当前 epoch
- 折射：`text.compact.v1`、`text.summarize.v1`

### 11.2 显像算法

按确定顺序构建 system fragments、对话消息、工具结果与来源；先保留最新输入和未完成调用，再保留最近消息，最后使用 summary。缓存键为 `(tail_seq, epoch, model_id, reducer_version)`。历史中已拔出的工具只显为叙述，不进入当前 tool schema。

### 11.3 折射与验收

compact/summarize 产生带 covered range/hash 的 `summary.created`。最终 prompt 不作为规范状态持久化；预算相同则输出字节级稳定；截断必须显式贡献原因。

## 12. Enso：Instruction、Skill、Memory 与 RAG 全息定影镜

### 12.1 Manifest

- ID：`org.tokmon.lens.enso`
- Surface：`model.context`、`ui.context-sources`
- 观察：`instruction.observed`、`skill.mounted`、`memory.*`、`rag.document-*`、`rag.index-*`
- 折射：`context.retrieve.v1`、`memory.propose.v1`、`rag.reindex.v1`

### 12.2 显像算法

每个 contribution 包含 source kind/id/hash、trust、sensitivity、token estimate 和内容。优先级是显式项目 instruction、用户 instruction、已挂载 skill、允许的 memory、RAG 结果；外部文档永远标记为 data。

### 12.3 折射与验收

retrieve 返回 document/chunk/hash/source；memory proposal 必须经 policy 后成为 `memory.accepted/rejected`；index 是可重建派生物。拔镜后下一次 view 零 contribution；prompt injection 文本不能升级为 instruction。

## 13. Techor：Tool 与 Code Mode 光能作动镜

### 13.1 Manifest

- ID：`org.tokmon.lens.techor`
- Surface：`model.tools`、`act.candidates`、`diagnostic.tool-decode`
- 观察：当前 tool schema、`model.tool-call`、code-mode frame、`tool.result`
- 折射：`tool.decode@tokmon.tool.decode.v1`

### 13.2 显像算法

把当前 LightPath 中可折射 ActPattern 显像为模型可见 schema；观察 structured tool-call，校验 tool name/schema/arguments/epoch，提出目标明确的规范化 Act。Calculator 的 `calculate` 映射为 `tool.calculate@tokmon.math.calculate.v1`。

### 13.3 折射与验收

显式 decode Act 生成 `tool.decoded` 或 `tool.decode-rejected`。生产主路径禁止解析自由文本命令标签；unknown tool、错误 schema、重复目标和旧 epoch拒绝；Code Mode 没有安全旁路；大结果只保留摘要和 artifact ref。

## 14. Fallen：Policy 与审批偏振滤光镜

### 14.1 Manifest

- ID：`org.tokmon.lens.fallen`
- Surface：`act.policy`、`ui.approvals`
- 观察：`act.proposed`、trust、workspace、policy、`approval.*`
- 折射：`approval.decide.v1`、`policy.evaluate.v1`

### 14.2 决策算法

规范化 Act 后计算 hash，按 deny → allow → ask 顺序匹配。ask 显像必须含原因、最小权限、影响范围、deadline、act hash 和 epoch；决定只对完全相同的 hash/epoch/generation 生效。

### 14.3 折射与验收

发出 `approval.granted/denied/expired` 或 `policy.evaluated`。policy 解析错误默认 deny；参数变化使批准失效；external irreversible 未批准时目标 Lens 永远收不到 Beam。

## 15. Cista：Secret 与脱敏遮光秘盒

### 15.1 Manifest

- ID：`org.tokmon.lens.cista`
- Surface：`act.secrets`、`diagnostic.redaction`
- 观察：SecretRef、敏感 schema、出口和 redaction report
- 折射：`secret.bind.v1`、`redaction.apply.v1`

### 15.2 算法

Surface 只显示 ref、provider、availability 和 purpose。bind 校验 exact act hash、target generation、purpose 和 deadline，产生短时 opaque binding id；输出清洗对 header、环境、URL、正文和日志应用 schema-aware redaction。

### 15.3 验收

明文不得出现在 Photon、Surface、spdlog、UI、artifact metadata 和 crash bundle；换代不继承 binding；redaction 失败阻止外发或 commit；测试扫描全部落盘字节。

## 16. Styx：执行隔离暗室

### 16.1 Manifest

- ID：`org.tokmon.lens.styx`
- Surface：`act.sandbox`、`ui.terminal`
- 观察：`act.admitted`、sandbox policy、process/worker 状态
- 折射：`process.exec.v1`、`process.cancel.v1`、`worker.launch.v1`、`wasm.invoke.v1`

### 16.2 折射算法

根据平台生成 SandboxPlan，包含 canonical cwd、argv、环境白名单、文件/网络范围、CPU/memory/output/deadline。stdout/stderr 用有界 ring 形成有序 chunk Photon；取消先 cooperative，再终止整个进程树；实际 SandboxStrength 必须写入结果。

### 16.3 验收

不可静默降级；shell 文本不替代 argv；cwd 必须由 Cove 验证；输出洪泛受限；worker 子树在 afterglow 后为零；Secret 仅在最终边界注入。

## 17. Chora：不可改写光感底片

### 17.1 Manifest

- ID：`org.tokmon.lens.chora`
- Surface：`fact.storage`、`diagnostic.capacity`
- 观察：tail、segment、blob、checkpoint、disk health
- 折射：`photon.export.v1`、`blob.put.v1`、`checkpoint.build.v1`、`archive.seal.v1`

### 17.2 持久化契约

Nyxia 中的 append gate 只能由当前 Chora generation 持有 writer token。提交事务分配 seq、补 parent、编码 canonical payload、计算全局 previous_hash/hash 并 fsync；成功后才通知 cursor。SQLite trigger 物理拒绝 UPDATE/DELETE，seq 不复用。

### 17.3 折射与验收

导出、blob、checkpoint 和 archive 都生成新结果 Photon。checkpoint 可删重建，不是事实源；换代使用短 commit barrier 交接 token；崩溃矩阵下无半个 Photon、无双 writer、哈希链可验证。

## 18. Tracket：因果记录与回放光路镜

### 18.1 Manifest

- ID：`org.tokmon.lens.tracket`
- Surface：`fact.integrity`、`ui.trajectory`、`ui.causality`
- 观察：所有 committed Photon、schema bundle、parent/hash/act refs
- 折射：`integrity.verify.v1`、`replay.create.v1`、`ray.fork.v1`、`trajectory.export.v1`

### 18.2 显像与折射

构建 timeline/DAG，校验 seq、id、parent、schema、epoch、previous hash、payload hash 和 caused-by-act。R0/R1/R2 回放只重建显像/请求/控制决定，不执行现实动作；R3 必须创建新 fork 和新 Act。

### 18.3 验收

未知 required schema、断链和跨 ray 非法 parent 必须报告；golden ray 忽略时间/随机 id 后语义严格相等；导出先经 Cista；源流绝不因 replay/fork 改变。

## 19. Nota：可观测性光谱分析仪

### 19.1 Manifest

- ID：`org.tokmon.lens.nota`
- Surface：`diagnostic.metrics`、`diagnostic.health`、`ui.diagnostics`
- 观察：engine step、view duration、Beam、queue、DB、worker、Snow、UI projection
- 折射：`telemetry.export.v1`、`profile.capture.v1`、`diagnostic.bundle.v1`

### 19.2 算法与验收

按 lens/generation/epoch/ray/act/beam 聚合 latency、errors、queue、bytes 和 health；不复制敏感 payload。exporter 失败只产生诊断，不阻塞 commit；profile/bundle 经 Fallen/Cista；遥测删除不影响恢复与回放。

## 20. Cove：Workspace、Git 与 Artifact 实景物镜

### 20.1 Manifest

- ID：`org.tokmon.lens.cove`
- Surface：`workspace.tree`、`workspace.diff`、`workspace.git`、`ui.artifact`
- 观察：workspace root、`fs.*`、`git.*`、`artifact.*`
- 折射：`fs.read/write/move/delete.v1`、`git.stage/commit/branch/merge.v1`、`artifact.create.v1`

### 20.2 安全算法

每次执行前 canonicalize 目标与根，逐段检查 symlink/reparse point；写前验证 precondition hash，写后重读并计算结果 hash。文件内容过大进入 Chora blob。delete/move/commit/merge 按策略要求审批。

### 20.3 验收

拒绝 traversal、根外路径、链接竞态和参数混淆；失败不伪造 `fs.changed`；Diff 是读回结果；并发修改触发 precondition conflict；artifact content-addressed。

## 21. Snow：CLI 与本地协议纯白投影幕

### 21.1 Manifest

- ID：`org.tokmon.lens.snow`
- Surface：`snow.protocol`、`cli.output`、`diagnostic.connection`
- 观察：protocol cursor、snapshot/delta、daemon health、`ui.intent`
- 折射：`snow.intent.v1`、`snow.cancel.v1`、`snow.reconnect.v1`

### 21.2 协议算法

Windows named pipe、Unix domain socket；固定 frame magic/version/flags/size/request id/cursor，payload 为 canonical CBOR。client 只能提交 intent。request id 幂等去重；cursor gap 必须 snapshot；human CLI 与 machine stream 分离。

### 21.3 验收

超大/半帧/非 canonical/major mismatch 拒绝；同用户边界；daemon 是唯一 Photon writer；desktop crash/reconnect 不影响 ray；Snow 不拥有 Agent 状态。

## 22. Termon：Slint 全息显像屏

### 22.1 Manifest

- ID：`org.tokmon.lens.termon`
- Runtime：desktop process，R2 handoff
- Surface：`ui.conversation`、`ui.trajectory`、`ui.code`、`ui.terminal`、`ui.approval`、`ui.lenses`
- 观察：Snow snapshot/delta、connection state
- 折射：把用户事件规范化成 Snow intent；不能提交 Photon。

### 22.2 显像算法

Slint 主线程只消费不可变 projection model；Snow I/O 在受管 worker 线程，16 ms 左右批量投递 event-loop。timeline 使用虚拟列表，terminal/diff 使用有界模型；Slint property 只是缓存。

### 22.3 验收

对话、轨迹、审批、Diff、终端、artifact、Lens inspector 和恢复状态闭合；CJK/IME/DPI/键盘可用；10k trajectory 与持续 stream 可交互；launcher 可启动候选 desktop、同步 cursor 后完成 R2 handoff。

## 23. Calculator：动态透镜参考实现

Calculator 不是十九个正式内置透镜之一，而是 C++/C ABI/Node.js/CPython 契约的同语义 fixture。

- ID：`org.tokmon.lens.calculator`
- Surface：`model.tools`，贡献 `calculate` 与 `tokmon.math.calculate.v1`
- 观察：当前 LightPath 和用户输入；不直接解析自由文本执行。
- 折射：`tool.calculate@tokmon.math.calculate.v1`
- 参数：`expression`，v1 支持有限、确定、无副作用的二元 `+ - * /`；除零和非法字符返回结构化错误。
- 结果：`tool.result@tokmon.math.result.v1`，含 expression 与数值。

Golden ray：

```text
user.input("128 * 4")
→ Janus proposes model.call
→ Rhea emits model.tool-call(calculate)
→ Techor proposes tool.calculate
→ Calculator emits tool.result(512)
→ Janus proposes next model.call
→ Rhea emits assistant.message
→ ray.darkened
```

## 24. 代码布局与完成定义

```text
lenses/<name>/
├─ <name>_lens.hpp
├─ <name>_lens.cpp
├─ lens.yaml
└─ tests/<name>_contract.cpp

lenses/common/
├─ lens_base.hpp/.cpp       # 仅通用校验与 emit helper，无按名称业务分支
└─ builtin_registry.cpp     # ID → constructor，只有注册关系
```

完成定义：十九个 Lens 均有独立类型；registry 不包含业务逻辑；runtime 使用 registry 创建内置 generation；每个动态库导出对应真实类型；每个 manifest 与 C++ manifest 相等；十九套 contract 加 Calculator golden ray 全部通过；删除任意一个 Lens 后其下一次 Surface/Act contribution 为零。

## 25. 已实现代码契约矩阵

下表不是愿景清单，而是当前 C++20 基线的逐镜实现索引。所有折射成功结果均通过 `RefractionBeam::emit` 形成 PhotonDraft，再由 Nyxia 的唯一 append gate 提交；表中的“结果 Photon”不能被原地修改或撤销。

| Lens | 独立实现 | 关键输入校验 | 结果 Photon / 现实边界 |
| --- | --- | --- | --- |
| Ignis | `ignis/ignis_lens.cpp` | desired 与 committed sequence、候选是否待处理 | `lens.verification-requested`、`mount.reconcile-requested`；epoch 发布留给 Nyxia |
| Lemon | `lemon/lemon_lens.cpp` | frame kind、1 MiB 上限、cursor 非负且严格递增 | `waveguide.frame-sent`、`waveguide.cursor-advanced`、`waveguide.reconnect-requested` |
| Iris | `iris/iris_lens.cpp` | opaque endpoint ref、operation、64 位十六进制语义 hash、幂等键、connection ref | `external.connection-opened/closed`、`external.call-completed`、`external.outcome-unknown`、`external.poll-completed` |
| Rhea | `rhea/rhea_lens.cpp` | model、prompt、token budget、幂等键；算术调用只接受有限二元表达式 | `model.requested`、`model.dispatched`、`model.tool-call` 或 `assistant.message`、`model.usage` |
| Janus | `janus/janus_lens.cpp` | terminal 状态、未完成 tool call、Surface/tool hash、连续动作振荡阈值 | 提出目标为 Rhea 的 `model.call`；折射产生 `ray.cancel-requested` 或 `user.steering` |
| Clotho | `clotho/clotho_lens.cpp` | workflow definition、node id 唯一、dependency 存在、DAG 无环、attempt | `workflow.step-completed`、`workflow.retry-requested`、`workflow.cancelled`；只提出最小序 ready node |
| Aya | `aya/aya_lens.cpp` | parent ray、预算非负、allowed acts、workspace mode、child ray | `child.started`、`child.joined`、`child.cancelled`；join 只携带 summary/artifact ref |
| Textus | `textus/textus_lens.cpp` | token budget、covered range、covered hash | `summary.created`；显像按最新输入、未完成调用、近因消息、summary 的次序裁剪 |
| Enso | `enso/enso_lens.cpp` | source id/hash、trust、sensitivity、query/memory/document 参数 | `context.retrieved`、`memory.proposed`、`rag.reindexed`；外部正文固定标为 data |
| Techor | `techor/techor_lens.cpp` | tool name、ActKind、schema、arguments、target | `tool.decoded`；不解析自由文本命令标签 |
| Fallen | `fallen/fallen_lens.cpp` | exact act hash、decision、epoch/generation 绑定 | `approval.granted/denied/expired`、`policy.evaluated`；未知决定默认拒绝 |
| Cista | `cista/cista_lens.cpp` | secret ref、purpose、exact act hash、target generation、lifetime | `secret.bound`、`redaction.applied`；日志和 URL/credential 模式在出口清洗 |
| Styx | `styx/styx_lens.cpp` | argv 数组、cwd、timeout、output 上限、取消状态 | 无 shell 插值地启动进程，产生 `process.completed/failed/timed-out` 与有界 stdout/stderr、隔离强度 |
| Chora | `chora/chora_lens.cpp` | storage root、blob bytes/ref、checkpoint/export/archive 参数 | 写入 SHA-256 内容寻址 blob，产生 `blob.stored`、`checkpoint.built`、`archive.sealed`、`photon.exported` |
| Tracket | `tracket/tracket_lens.cpp` | sequence、ID、parent、hash 形状、replay level | `integrity.verified`、`replay.created`、`ray.forked`、`trajectory.exported`；R3 才允许现实重演 |
| Nota | `nota/nota_lens.cpp` | telemetry/profile/bundle 参数与诊断边界 | `telemetry.exported`、`profile.captured`、`diagnostic.bundle-created`；不复制敏感正文 |
| Cove | `cove/cove_lens.cpp` | canonical root/path、根内约束、precondition hash、写后 hash、Git argv | 真实 read/write/move/delete 与 Git stage/commit/branch/merge；产生 `fs.*`、`git.*`、内容寻址 `artifact.created` |
| Snow | `snow/snow_lens.cpp` | intent/cancel/reconnect 结构、request id、cursor | `snow.intent-forwarded`、`snow.cancel-observed`、`snow.reconnect-requested`；传输使用 named pipe/Unix socket 与 canonical CBOR |
| Termon | `termon/termon_lens.cpp` | UI intent kind、payload 与目标；projection 有界并脱敏 | `ui.intent-forwarded`；只显像会话、轨迹、代码、终端、审批和 Lens 状态 |
| Calculator | `calculator/calculator_lens.cpp` | 单个二元 `+ - * /`、有限数值、除零 | `tool.result@tokmon.math.result.v1`，作为四种承载形态的契约样例 |

## 26. 通用基类与独立性边界

`lenses/common/lens_base.*` 只实现以下机械能力：保存 manifest、stop 状态、ActPattern 匹配、稳定 Act ID/幂等键、统一 identification contribution 和 Beam emit。它不读取 Lens 名称并执行条件分支，也不持有任何业务状态。`builtin_registry.cpp` 只把 short id 映射到二十个具体构造函数；动态库入口同样先调用这个构造函数，再把真实对象适配到稳定 C ABI。

因此“独立实现”具体意味着：

1. 每个目录有独立 final C++ 类型，构造自己的 manifest，并拥有自己的 `view/refract` 算法；
2. 删除某个 `<name>_lens.cpp` 会让对应 target 链接失败，而不会退回通用占位逻辑；
3. 二十个 DLL 的 `tokmon_lens_entry_v1` 分别返回真实身份，并由自动测试逐个装载验证；
4. manifest YAML 和 C++ manifest 在测试中逐字段比对，避免文档、声明和实现漂移；
5. `request_stop()` 后再调用 `view` 必须返回 `cancelled`，用于证明拔镜后的零贡献边界。

## 27. 运行时组合与热插拔实现

`TokmonRuntime::reconcile()` 执行一次完整换镜事务：

```text
读取用户级/项目级 .tokmon/light-path.yaml
→ 追加 config.light-path-observed
→ 为每个候选解析承载形态并计算 artifact 内容哈希
→ 校验 manifest id/runtime/ABI/entry/permission
→ 构造新 generation；Worker 先完成 hello/ready
→ 空 PhotonWindow 执行 dark-lane view
→ 追加 lens.candidate-verified（失败则 candidate-rejected）
→ 在隔离 LightPath 上验证 ID 与 ActPattern 唯一性
→ 追加 mount.epoch-committed
→ 单次 atomic<shared_ptr> 发布完整 E+1 快照
→ 旧 generation 停止接收 Beam、等待在途 Beam、request_stop
→ 追加 lens.afterglow-started/completed
```

发布前失败时 active LightPath 保持 E；发布后发现问题时不能改回 E，必须把旧 artifact 作为新候选发布 E+2。C ABI 动态库在进程内换代；Node.js 与 CPython 强制通过 `WorkerLensProxy` 和 `tokmon-lens-worker` 换代。脚本 artifact 的 `runtime.entry` 必须位于 artifact 根内，运行时发行版来自用户 `.tokmon/runtimes/node` 或 `.tokmon/runtimes/cpython`，不从 PATH 浮动解析。每个 Worker generation 有独立进程树，deadline、取消或协议错误可终止整棵树。

## 28. 自动验收映射

当前测试由四层组成：

- 二十个 `lenses/<name>/tests/*_contract.cpp`：YAML/C++ manifest 等价、确定性显像、声明 channel、未知 Act pass-through、stop 后零贡献；
- `builtin_lens_tests.cpp`：十九镜顺序与唯一性、二十个真实 DLL 身份、每镜一个声明折射场景，以及 Styx/Cove/Chora/Clotho 的现实边界；
- `core_tests.cpp`：canonical CBOR、SQLite 物理 append-only/hash chain、LightPath 原子发布、C ABI dark lane、三拍 Calculator golden ray、换镜生命周期 Photon；
- Node.js/CPython SDK 与 Worker proxy：真实外部解释器进程、hello/ready、view、refract、host Beam emit、shutdown。

所有测试都以 C++20 编译；Slint 只用于 Termon 桌面显像，不改变任何 Lens 语义或 Photon 事实源。
