# “折光因果透镜可拆卸编程范式”（RCLD）与 tokmon-n 工程：对照 Cordis 体系的客观分析报告

> 分析日期：2026-08-25
> 分析对象：
> - 理论文档：`detachable-refractive-causal-lenses-paper.zh.md`（RCLD 论文）、`everything-is-a-lens-paper.zh.md`（EiaL 论文）、`tokmon-lens-architecture-explained.zh.md`（白话精析）、`advise.md`、`20.md`
> - 工程实现：`tokmon-n`（本地 HEAD `e243077` "update ui"，另有 5 个未提交修改）
> - 对照系：Cordis 论文《A Programming Paradigm for Spatiotemporal Composability》（Shi, Zhang, Cui — 北京大学 & DeepSeek-AI，88 页）、`cordis` 框架源码（cordiverse/cordis，core 4.0.0-rc.8）、`deepseek-harness` 源码（deepseek-ai，v0.1.1-rc.2）
> - 性质：架构与实现审查。非同行评审、非形式化验证报告、非安全认证。

---

## 1. 执行摘要

### 1.1 一句话结论

**RCLD 是一个方向正确、叙事华丽但形式化与实验证据均不达“范式/定理”标准的工程哲学；它对 Cordis 的三点批判有一半以上是对 Cordis 论文的误读；而 `tokmon-n` 是一个远超"PPT 工程"水准的真实原型——核心机制（append-only 因果光子库、epoch 化原子热装卸、afterglow 退役、20 个真实透镜、85 项含真实 I/O 的测试）落地扎实，但其论文宣称的极限指标（0.18ms 微秒级拆卸、100% 零幻觉、<50MB 内存锁定）在仓库中没有任何测量支撑。**

### 1.2 五条最重要的判断

| # | 判断 | 依据 |
|---|---|---|
| 1 | **RCLD 的工程内核（Fact→Lens→Surface/Act→Fact）是有价值的架构综合**，把事件溯源、CQRS 投影、functional core / imperative shell、capability-based security 组合成适合 LLM Agent 的形态 | 去比喻后每个概念都有成熟先例（见 §3.1），组合本身有新意 |
| 2 | **RCLD 的形式化部分存在实质性缺陷**：“零残留认知卸载定理”要么平凡真、要么按其自身定义为假；GetPut 公理与自定义的 `refract(s,b) := s ⊗ ⟨Photon(b)⟩` 直接矛盾 | 论文第 2.2/3.3 节自洽性检查，见 §4.2 |
| 3 | **对 Cordis 的“三大鸿沟”批判不够公允**：Cordis 论文 §6.1 明确用 system boundary 区分 acquisition/emission 并讨论 withholding 与 compensation（即承认物理不可逆）；§6.5 明确分析依赖环的可预测失活与分解策略；“投机隔离”一条部分成立 | Cordis PDF 第 67–71 页原文核对，见 §5.2 |
| 4 | **tokmon-n 不是 vaporware**：约 42,500 行 C++/Slint 代码、289 个源文件；PhotonStore 有 SHA-256 哈希链 + SQL trigger 物理禁止 UPDATE/DELETE；reconcile 实现了清单校验→签名/SBOM 验证→权限不扩张→拓扑排序→先落盘后原子发布；afterglow 卸载序列真实等待在途光束归零；85 cases / 2965 checks 全绿（2026-08-25 本机复跑） | 子代理全仓调研 + 本机验证，见 §6 |
| 5 | **论文评测章节（第 7 章）整体不可复现**：仓库中无任何 benchmark 代码；论文第 6 章“无 Mock”示例自身硬编码了模型响应 `"CALL_TOOL:calculate:128 * 4"`，“验证 2”打印的是预期结果而非断言计算结果 | grep 全仓无 `benchmark` 命中；论文第 6 章代码逐行核读 |

### 1.3 一个容易被忽略的客观事实

**deepseek-harness（被 RCLD 论文点名的“现代 Agent 运行底座”）在语义上已经收敛到了 RCLD 所主张的一半主张。** 其官方架构文档明文规定：

> "The session log is the source of the context the model sees. `deriveMessages()` projects model history from it... **Model-visible means logged.**"
> —— `deepseek-harness/docs/architecture.md:94-96`

即：只追加事实日志 + Prompt 纯投影派生 + 注册即可逆效应（Cordis effect）。RCLD 相对于这条已经产品化的路线，真正的差异化只剩：**放弃逐操作逆函数（改用 epoch 整代切换）、结构化 Act 准入（admission）、afterglow 在途光束排空、多语言透镜进程边界**——这些是有价值的工程增量，但不是“代际替代”。

---

## 2. 三方体系速览

| 维度 | Cordis（论文+框架） | deepseek-harness | RCLD 论文 + tokmon-n |
|---|---|---|---|
| 定位 | 动态组合的元理论 + TypeScript 元框架 | 基于 Cordis 的产品级 Agent harness | Agent 操作系统的光学范式 + C++20 参考实现 |
| 时间可组合（卸载） | Revertible Effects：每操作带逆元，LIFO 倒序恢复（`fiber.ts:282` `disposables.reverse()`） | 继承 Cordis："Registrations are effects"，框架托管 dispose | 不写逆函数：epoch 整代原子切换 + afterglow 排空在途光束（`runtime.cpp:708-732`） |
| 空间可组合（依赖） | Reactive Coeffects：inject 声明依赖，上下文变化触发激活/停用通知；Fiber 状态机 PENDING→ACTIVE→UNLOADING | `inject=['tools']` 服务注入 + isolate + patch 叠加 | LightPath 快照 + manifest 权限声明 + 拓扑排序 + 权限不得跨代扩张 |
| 模型可见上下文 | （框架层不管；由上层决定） | append-only SessionEvent 日志，`deriveMessages()` 投影 | Photon 光流 + Lens.view 即时折叠投影，无 Prompt 缓存 |
| 形式化深度 | 88 页：效果/余效果系统 → 演算 → Preservation/Temporal/Spatial Composability/Progress/Confluence 五组定理带证明 | — | 两篇白皮书：定义+公理+4 条定理，证明为一至两段文字，无机器可检验规范 |
| 实现规模 | cordis monorepo（core/utils/timer/hmr/loader…，vitest 全覆盖） | ~50 包 pnpm monorepo + Python SDK | ~42.5K 行 C++/Slint，85 测试用例 |
| 生产验证 | Koishi 案例研究（论文 §5.3）；dsh 产品化 | DeepSeek 官方开源产品 | 无外部用户；本机测试通过 |

---

## 3. RCLD 范式分析

### 3.1 去掉光学隐喻后的工程实质

| Tokmon 术语 | 工程含义 | 最接近的既有概念 |
|---|---|---|
| 因果光流 Φ / Photon | 带 schema、来源、因果父指针、哈希链的不可变只追加事件 | Event Sourcing、audit log |
| Lens = ⟨view, refract⟩ | view：从事件窗口派生投影（Prompt/UI）；refract：执行动作并追加结果事件 | CQRS projection + command handler；functional core / imperative shell |
| 组合即叠镜 | 投影是纯函数管道 fold | Unix 管道 / React `UI=f(state)` / Git DAG |
| 卸载即拆卸 | 移除投影函数项，重新 fold 即无该模块贡献 | 纯函数视图天然性质（React 卸载组件不残留 DOM） |
| Scope Token 一键断电 | 资源生命周期绑定到组件租约 | RAII / capability-based security |
| 单向直线光缆 O(N) | 禁止网状事件总线，线性 pipeline | pipeline architecture |
| 自然停机律 | 模型无工具调用则循环退出 | ReAct 循环终止条件 |

**评价**：这套词汇系统的真正贡献不在发明新理论，而在把若干成熟纪律**强制统一到单一心智模型里，并针对 LLM 的注意力特性给出动机**——“卸载插件必须从模型注意力场中抹除其存在证据”这个问题意识是真问题，且比传统 dispose 清理的表述更贴合 Agent 场景。`advise.md` 中甚至自我反省过“过度形式化与概念堆砌”的陷阱，最终收敛到 Fact-Lens-Act 三原语——这个自我纠偏过程说明设计者清楚什么是好的系统哲学。

### 3.2 形式化部分的实质性缺陷

以 RCLD 论文（`detachable-refractive-causal-lenses-paper.zh.md`）为对象逐条检查：

**(1) GetPut 公理与 refract 定义直接冲突。**
定义 3 规定 `refract(S, b) := S ⊗ ⟨Photon(b, parent=last(S))⟩`（第 192 行），即任何折射都追加一颗新光子。而三大守恒定律第一条要求 `refract(s, view(s)) ≡ s`（第 205 行）。只要 `view(s)` 非空，`s ⊗ ⟨p⟩ ≠ s`。除非引入一个未声明的商等价 `∼` 使“追加了已见像的光子”视为恒等，否则公理在其自身定义下不成立。

**(2) “零残留认知卸载定理”是平凡真或假，取决于解读。**
- 平凡真解读：`(𝔏 \ {𝓛_P}).view(Φ) ≡ 𝔏_clean.view(Φ)`——两边本来就是同一个透镜集合，等式恒成立，不含信息量；
- 非平凡解读（论文想要的）：“拆卸后大模型视界中关于 𝓛_P 的任何上下文引用完全消失”——**这按其自身框架为假**：Φ 是只追加光流，插件活跃期写入的 TOOL_CALL/TOOL_RESULT 光子永远在 Φ 里，任何折叠全部历史的 view（如 Textus 组装 Prompt）都会继续投影出这些历史。要做到论文宣称的效果，必须依赖一个额外的、未被定理覆盖的机制——文档 Q&A 里提到的“Textus 将历史旧记录降级为只读文本并剥夺工具声明”。也就是说：**结论的正确性来自 Textus 的过滤策略，而不是来自“移除透镜”这一动作本身；定理的证明恰恰回避了这一点。**

**(3) 进展/合流性定理遗漏前提。**
定理 3（无死锁进展）给出的时限 `Σ latency(ℒ_i) + latency(LLM)` 把 LLM 当作有限延迟的确定性函数；定理 4 断言终态正规形全局唯一——而 LLM 是非确定外部神谕，同一输入两次采样可以产生不同工具调用序列，合流性（confluence）在外部非确定性存在时需要完全不同的处理（这正是 Cordis 用 30+ 页处理 withdrawal/iteration/asynchrony/failure 的原因）。RCLD 对此一笔带过。

**(4) 演算不封闭。**
8 条规约规则中 [O-WaveMerge]（波前合并）依赖未定义的 `Merge(Φ, Φ_shadow)` 合并算法与 `ProofVerified` 判据；[O-Compaction] 依赖未定义的 `Compress(Φ)` 且与“历史不可磨灭”公理存在张力（压缩后的光流还是原来的 Φ 吗？）。tokmon-n 代码中这两者实际上都缺席（无影子分支合并；只有固定 4096 条 photon window 上限，无压缩算法）——代码比论文诚实。

### 3.3 实验章节的证据学问题

论文第 7 章给出了极具说服力的表格（VSCode 残留 4820 tokens / 幻觉率 34.2%；Cordis 120 tokens / 2.1%；RCLD 0 tokens / 0.00%；热拆卸 4200ms vs 1800ms vs 4.2ms vs **0.18ms**）。经核查：

1. **整个工作区不存在任何 benchmark 代码或原始数据**（全仓 grep 无命中）；
2. 对照组的数字（尤其 VSCode 34.2%、Cordis 2.1%）没有任何可追溯的实验协议、负责任何置信区间；
3. 论文第 6 章自称“完整无 Mock”，但其演示引擎硬编码 `model_response = "CALL_TOOL:calculate:128 * 4"`（模拟大模型），“验证 2”直接 `std::cout` 打印预期文案而非断言重投影结果——**示例自身就是 Mock 与占位验证**；
4. 0.18ms 的量级宣称与 tokmon-n 实际卸载路径（5ms 轮询 + 最长 2s deadline 排空在途光束，`runtime.cpp:718-721`）在数量级上不一致。

结论：**第 7 章应视为愿景性修辞，不能作为证据引用。** 这一点 tokmon-n 自己的 `docs/design-rs.md:2397` 也承认——“未经基准验证的绝对性能/零幻觉声明”被列为禁止事项，说明工程侧文档与论文侧表述存在自觉的切割。

### 3.4 对 Cordis 三大批判的公允性核对

RCLD/EiaL 论文声称 Cordis 存在“三个难以逾越的鸿沟”。逐一对照 Cordis 原文：

| RCLD 的批判 | Cordis 原文事实 | 判定 |
|---|---|---|
| ① 物理发射绝对不可逆，Token 算力/HTTP 支付无法靠左逆元倒流 | Cordis §6.1 明确定义 system boundary：emission 作用于边界外、"acts as idΓ, neither tracked nor recovered"；并提出 withholding（输出提交问题）与 compensation（补偿动作，如退款）两条正路，且指出补偿的组合仍服从 LIFO | **大部分误读**。Cordis 从未声称外部发射可逆；RCLD 批判的是一个 Cordis 自己已经明确排除的主张 |
| ② 多代理双向协作违反依赖图严格无环假设 | Cordis §6.5 承认环会导致组件永久失活，但强调这是**从依赖声明即可静态预测**的（好于运行时死锁），并给出分解方法论（n 个互依组件最坏二次方个集成组件），同时承认开发者体验代价 | **一半成立**。“DAG 约束带来建模负担且可能平方级膨胀”是真实代价；说 Cordis“违反假设即崩溃”不准确 |
| ③ 投机探索缺乏世界线隔离与无损合并 | Cordis 提供 `ctx.extend()` 影子上下文与 `isolate()` 服务隔离，但没有跨“世界线”的事件级 fork/join 合并语义 | **基本成立**。这确实是 RCLD/BeamSplit 意图上有差异化的点——尽管 tokmon-n 目前也没实现它（Aya 只有 child-ray，无三路波前合并） |

**总体**：RCLD 把“Cordis 承认边界并给出缓解方案”叙述成“Cordis 无法处理”，属于论战性修辞。更准确的表述应该是：**两者对“时间”的处理哲学不同——Cordis 相信细粒度可逆（尽力让更多东西可逆），RCLD相信粗粒度不可变（承认不可逆，用整代切换 + 只追加历史绕开回滚）**。后者在开放世界场景下确实更稳健，但这是一种取舍，不是严格优势。

### 3.5 命名/口径的自相矛盾

- 文档称“20 组精密透镜”并列出 Nyxia 为第一号透镜，但 tokmon-n 代码中 Nyxia 是微内核引擎目录（不可拆卸基座），正式光路是 **19 业务镜 + calculator 示例镜**（`builtin_registry.cpp::official_lens_order()` 共 19 项）；
- 论文称 C++23 实现，工程实际为 **C++20**（`CMakeLists.txt`，且 `IMPLEMENTATION-REPORT.md` 已自我修正）；
- 论文称 Termon 基于 Skia 渲染引擎 + White 声明式 DOM，代码实际使用 **Slint 1.17.1**（`.deps/slint`，14 个 .slint 文件），全仓无 Skia/White/Speculum 任何痕迹；
- EiaL 论文称“8 套核心光线追踪算法”、RCLD 称“波前快照增量坍缩算法”，代码中均无对应物。

---

## 4. tokmon-n 工程实现分析

### 4.1 工程概况

```text
tokmon-n/
├── nyxia/        微内核引擎库（~4,220 行）：engine/light_path/mount/runtime/storage/loader/worker
├── lenses/       19 业务透镜 + calculator，每镜含 *_lens.cpp + lens.yaml 清单 + 契约测试（~8,181 行）
│   └── common/   lens_base / builtin_registry / C ABI 入口 / http_client / process_runner / pty / prometheus / schema_validator / secret_store
├── protocol/     snow_protocol / snow_transport(named pipe+stdio) / slash_commands / daemon_lifecycle（914 行）
├── sdk/          C ABI 头文件 + C++29 头 + Node.js(worker.mjs) + CPython SDK（~1,773 行）
├── apps/         统一入口 tokmon + tokmond daemon + lens-worker + Slint 桌面端（~13,850 行，其中 Slint UI ~7,950 行）
├── tests/        core_tests(23 例) + builtin_lens_tests(42 例) + 20 个逐镜契约 + 真实进程 fixtures（~2,660 行）
├── config/       light-path.yaml 等
└── build/        四个成熟构建树；bin/ 下 tokmon.exe/tokmond.exe/20 个透镜 DLL 齐全
```

合计约 **289 个 C/C++ 源文件、42,500 行**（不含 build/.deps）。构建依赖均为真实第三方（tl::expected、chyaml、chjson、SQLite amalgamation 3.49.1、chtest、Slint 1.17.1），FetchContent 可复现。

### 4.2 核心机制的落地质量（本次重点核验）

**（1）持久化是真的 append-only。**
`nyxia/storage/photon_store.cpp:117` 设置 `PRAGMA journal_mode=WAL;`；数据库层用 trigger 拒绝对已提交光子的 UPDATE/DELETE；SHA-256 哈希链（previous_hash）；测试 `"Photon store is hash chained and physically append-only"` 用原生 sqlite3 API 验证篡改必失败。**这与论文的“唯光不灭”公理是代码级对应，是全工程最扎实的一点。**

**（2）动态装卸是真实的、分阶段的。**
`TokmonRuntime::reconcile()`（`runtime.cpp:600-734`）流程：读用户级+项目级 YAML → dark lane 中逐镜 stage（支持 builtin / C ABI dlopen / native worker / Node.js / CPython 五种运行时）→ 校验 manifest/ABI/hash/lens-lock/HMAC-SHA256 签名/schema bundle/SBOM 证据 → **权限不得相对上一 generation 扩张** → 空 PhotonWindow 试跑 view → 依赖拓扑排序（成环拒绝）→ 先落盘 `mount.epoch-committed` 光子再原子发布新 `LightPathSnapshot`（`std::atomic<shared_ptr<const Snapshot>>`）。这个顺序（先持久化证据、再切换可见状态）符合 WAL 纪律。

**（3）卸载不是“拔指针”，而是 afterglow 排空协议。**
`runtime.cpp:708-732`：记 `lens.afterglow-started` → `beams_.stop_generation()` 拒绝新光束 → 以 5ms 轮询等待在途光束归零（deadline 2s）→ `request_stop()` → 记 `lens.afterglow-completed`。相比论文“一微秒旋出镜片”的浪漫化描述，这个实现**更慢但更诚实**：它承认在途 I/O 不能瞬间消失。HMR 有专项测试（calculator 从磁盘 DLL 热替换为 builtin，断言 epoch+1 与 artifact_hash 变化）。

**（4）“零残留”在工程文档中被正确降格。**
`design-rs.md:89`：“不承诺 LLM 永远不会‘幻想’一个不存在的工具名；系统只保证不存在宿主侧残留工具 Schema，并拒绝未在当前 epoch 注册的调用”；`:2604` 明确列出误解风险。**这个降格后的承诺（宿主 Surface/route/resource 无残留 + 结构化 admission 兜底拒绝幻觉调用）才是真正可辩护的产品契约——即使模型幻觉出旧工具名，运行时也会拒绝执行。这其实比论文的“数学上杜绝幻觉”更有工程价值。**

### 4.3 20 个透镜的实现完成度

注册表共 19 业务镜 + calculator。**没有一个是返回硬编码数据的空壳**，但完成度分层明显：

| 层级 | 透镜 | 评估 |
|---|---|---|
| **厚重真实**（400-700 行，含专项测试） | Iris（MCP stdio/HTTP client + LSP，配真实 Python fixture 往返测试）、Rhea（多厂商网关、chhttp SSE 解析与传输、指数退避×5 重试、SecretBuffer 清零）、Clotho（YAML DAG 校验/环检测/fan-out/补偿 Act）、Enso（SKILL.md 引用链加载、RAG 混合检索、append-only 记忆提案链）、Techor（工具目录+JSON Schema 强校验+Code Mode）、Styx*（argv 无 shell、Job Object/PTY 真 PTY、有界输出、协作取消；WASM/容器 adapter 已编码但 fail-closed）、Cove（实体扫描、guarded write、路径逃逸防护、Git worktree 证据）、Chora（内容寻址 Blob、DPAPI 加密） | 核心能力可信 |
| **中等真实** | Lemon（背压/游标/订阅记账）、Janus（turn/step 状态机重建）、Aya（子 ray fork、真实 Git worktree 隔离测试）、Textus（token 预算/滑窗/截断）、Nota（OTLP 导出+Prometheus 回环 server，真实 collector 测试）、Tracket（哈希链校验/分级回放）、Fallen（deny>allow>ask 优先级）、Cista（Windows Credential Manager、macOS Keychain、Linux Secret Service + 一次性绑定） | 可信但范围较窄 |
| **薄壳委托** | Ignis（41 行，投影 desired/committed 差异并 propose reconcile；实际装卸逻辑在 runtime）、Snow（38 行，实体在 protocol/snow_protocol.cpp）、Termon（95 行，13 个 ui.* 通道投影）、calculator（62 行参考实现） | 设计上职责外移，不算缺陷但与文档口径需对齐 |

Styx 的 fail-closed 策略值得表扬：本机没有 Wasmtime/Docker 时**拒绝而不静默降级**到宿主直跑，并有测试 `Styx refuses unavailable WASI and container adapters without host fallback` 锁定该行为。

### 4.4 测试证据

本机复跑（2026-08-25，现有 `build/windows-msvc-release` 二进制）：

```text
[chtest] cases=85 subcases=0 checks=2965 failures=0 time≈10.5s  ALL PASSED
+ sdk-node-contract Passed
+ sdk-cpython-contract Passed
```

亮点：fixtures 全部是**真实进程**——Python MCP stdio fixture、LSP fixture、SSE 模型服务器、HTTP capture server、PTY fixture；Snow CLI 有 3 秒真实断线重连测试；Node.js/CPython 经 WorkerLensProxy 端到端。这种“无 Mock”纪律在个人/小团队项目里罕见，**恰好是对论文“无 Mock”宣称在工程层面的真正兑现**——讽刺的是兑现得比论文正文更好。

保留意见：测试结论限于 Windows + 现有构建产物；工作区另有 5 个 dirty 文件未做 clean rebuild；无模糊/并发压力/长稳测试；无任何性能基准。

### 4.5 论文宣称 vs 代码现实差距总表

| 论文/文档宣称 | 代码现实 | 差距定性 |
|---|---|---|
| append-only 哈希链因果光流 + SQLite WAL | ✅ photon_store.cpp，trigger 禁改，测试实证 | 相符 |
| 透镜 = 纯 view/refract 双向函数 | ✅ ILens 接口 + 契约测试校验确定性 view | 相符（Styx 是唯一声明持态透镜，manifest 如实标注） |
| 插件热装卸/HMR | ✅ reconcile 五阶段校验 + epoch 原子发布 + DLL 热替换测试 | 相符且比论文严谨 |
| 零残留卸载定理 | ⚠️ 实现为 admission 拒绝未注册调用 + design-rs.md 主动降格承诺 | 论文夸大，工程务实 |
| 0.18ms 微秒级拆卸 / <50MB 内存 / 100% 零幻觉 | ❌ 无任何测量代码与数据；afterglow 上限 2s | **不可采信** |
| C++23 | C++20 | 小差距，已自我修正 |
| 20 透镜（Nyxia 为其一） | 19 镜 + calculator；Nyxia 是引擎 | 口径不一 |
| Styx=E2B 远程容器/WASM 沙箱 | WASM/Docker adapter 编码但 fail-closed 未实测；无 e2b 痕迹 | 部分相符 |
| BeamSplit 影子光流 + WaveMerge | ❌ 仅 Aya child-ray，无世界线合并 | 缺席 |
| O-Compaction 波前坍缩 | ❌ 固定 4096 photon window 上限 | 缺席 |
| Termon=Skia+White 声明式 DOM | Slint 1.17.1 | 文档过时/虚构 |
| Enso 向量 RAG | ✅ chunk/embedding 混合检索 + tombstone | 相符（简化版） |
| MCP/LSP | ✅ Iris 真实 client + fixture 往返测试 | 相符 |

---

## 5. 三方横向对比与定位

### 5.1 理论层面：三种时间哲学

- **Cordis**：细粒度可逆。相信“大多数修改可以被逆元精确撤销”，为此付出每操作携带闭包逆元的作者税，换取卸载的精确性。其元理论（Preservation/Progress/Confluence）经过完整证明，是目前动态组合领域最严肃的形式化之一。
- **RCLD/Tokmon**：粗粒度不可变。承认开放世界不可逆，把一切历史固化为事实流，用“整代切换 + 重投影”替代“逐操作回滚”。这在哲学上接近数据库的 immutable snapshot + 版本化 schema evolution。
- **客观判定**：两者解决的是同一问题的不同剖面。Cordis 强在“组件间共享可变服务的协调卸载”（服务注册、事件监听、依赖级联），RCLD 强在“模型可见能力的代际一致性”（epoch 内 Schema 全集稳定、admission 兜底）。**RCLD 不是 Cordis 的替代，而是把 Cordis 类框架中最容易出问题的“Prompt/能力面”部分换成了另一种更保守的机制。** 若二者结合（Cordis 管服务装配、RCLD 管认知表面），理论上互补性大于竞争性。

### 5.2 工程层面：三个实现的成熟度

| 维度 | cordis | deepseek-harness | tokmon-n |
|---|---|---|---|
| 语言/运行时 | TypeScript/Node | TS/Node ≥22 + Python SDK | C++20 原生 + 多语言 worker |
| 生态位 | 元框架（被 dsh vendored，固定 SHA pin + 18 处生产加固） | 产品（CLI/Web UI/SDK/沙箱/技能/compaction/subagent 约 50 包） | 参考实现/原型（daemon+CLI+桌面） |
| 卸载保证 | LIFO effect 恢复，vitest dispose 测试族 | 框架托管 + 生产补丁修复 fiber.ts 重入释放缺口 | afterglow 排空 + epoch 原子发布，测试实证 |
| 认知表面 | 不管 | append-only 日志 + deriveMessages 投影 + "Model-visible means logged" 运行时不变量断言 | Photon 流 + view 折叠 + admission 拒绝未注册调用 |
| 外部验证 | Koishi 案例（论文 §5.3）、dsh 产品采用 | DeepSeek 官方开源 | 无 |

**关键观察**：deepseek-harness 的会话语义（append-only log + 纯投影派生 Prompt + 日志不变量断言）与 RCLD 的 Fact-Lens 核心**几乎同构**。这意味着 RCLD 论文引言里“现代 Agent 底座因有状态插件而必然崩溃”的叙事，对其点名的对象并不公平——dsh 恰恰证明了“Cordis 可逆效应 + 只追加日志投影”这条路线在生产中可行。RCLD 的净增量收窄为：① 不要求插件作者写逆函数（对 C++/原生生态友好，dlopen 场景写逆函数确实困难）；② 结构化 Act 准入与权限不扩张约束；③ 多语言透镜进程隔离与签名/SBOM 供应链校验。这些都是实打实的工程价值，但属于**渐进创新**而非范式革命。

### 5.3 风格层面的一针见血

- Cordis 论文是标准学术文体：定义—定理—证明—案例，克制、可复核、坦承 open problems（§6 整章都是）；
- tokmon-n 工程文档（DESIGN.md/design-rs.md/IMPLEMENTATION-REPORT.md）延续这种克制，甚至专门设立“禁止未经基准验证的绝对声明”条款；
- 而 RCLD/EiaL 两篇白皮书与 advise.md 则充满“绝对零残留”“彻底杜绝”“代际碾压”“降维打击”“大圆满”式修辞，并虚构了不可复现的对照组实验。

**同一个项目内部同时存在两种话语体系，且互相矛盾——这是当前 tokmon-n 文档资产最大的负债。** 新读者若先读论文再读 DESIGN.md 会产生严重的预期偏差。

---

## 6. 总体评价

### 6.1 分项评分（10 分制审查量表，非行业标准）

| 对象 | 维度 | 评分 | 一句话理由 |
|---|---|---:|---|
| RCLD 思想 | 问题选择 | 8 | 注意力污染/动态卸载/审计边界是真问题，动机论述出色 |
| RCLD 思想 | 概念原创性 | 5 | 主体可追溯到 event sourcing/CQRS/fc-is/capability security；组合与命名有新意 |
| RCLD 论文 | 形式化严谨性 | 3 | 公理自冲突、定理平凡化、演算关键规则未定义 |
| RCLD 论文 | 实验可信度 | 2 | 无协议、无数据、无对照组出处；示例自带 Mock |
| RCLD 论文 | 对相关工作公允性 | 3 | 对 Cordis 三大批判两条属误读 |
| tokmon-n | 架构一致性 | 8 | 三平面/epoch/admission/afterglow 自洽闭环 |
| tokmon-n | 实现覆盖度 | 6.5 | 核心链路真实；资源托管边界、WASM/容器、合并算法缺位 |
| tokmon-n | 测试证据 | 7.5 | 85/2965/0 全绿 + 真实 I/O fixtures，但限单平台单构建 |
| tokmon-n | 文档健康度 | 4 | 工程文档优秀，论文层严重虚饰且与工程文档互相打架 |
| tokmon-n | 生产成熟度 | 4.5 | 高级原型；不宜按论文口径对外背书 |

### 6.2 建议

**给范式/文档：**
1. 将两篇论文降格重标为 *whitepaper / research agenda*，删除“已严格证明”“100%/0.18ms/34.2% 对照表”等不可证伪表述；或补齐可复现的实验协议后再发表；
2. 把“零残留定理”改写为可检验命题：“卸载后，当前 epoch 的 Surface 投影不含该透镜 Schema，且运行时拒绝其调用”——这在 tokmon-n 里已经有测试支撑，完全可以理直气壮地写；
3. 修正 20/19 口径、Skia→Slint、C++23→C++20 等事实错误，统一论文层与工程层的术语权威级（DESIGN.md 已规定优先级，论文应显式服从）。

**给工程：**
4. 补一个最小 benchmark harness（mount/unmount 延迟分布、fold 投影耗时、内存 RSS 曲线）——哪怕数字不如论文好看，可信度的提升也是数量级的；
5. 优先落地两个论文已宣传但代码缺席的能力：影子光流 fork/merge（自进化试跑的前提）与光流 compaction（长会话内存上限的实际手段）；
6. 把 Styx 的 WASM/container adapter 从 fail-closed 推进到 live test，或在所有文档中显式标注“规划中”。

### 6.3 结论

**作为思想**：RCLD 提供了一套对 LLM Agent 运行时颇有解释力和指导性的工程纪律——事实唯一、投影纯净、能力代际化、副作用准入化。它值得继续演进，但应以“架构模式”而非“已证明的新范式”的身份演进。

**作为工程**：tokmon-n 是这次调研中最大的惊喜。它没有兑现论文的极限指标，却兑现了一件更重要的事——**证明这套理念可以在真实操作系统、真实进程边界、真实第三方协议（MCP/LSP/SSE/PTY/Prometheus）上以无 Mock 方式跑通并被测试钉住**。以参照系衡量：它的理论对手 Cordis 有更深的数学和更大的生态，它的同辈 deepseek-harness 有更完整的产品面；但 tokmon-n 在“原生性能语言中的透镜式 Agent 运行时”这个具体生态位上，目前是一个认真、诚实（就其工程文档而言）、可继续投入的原型。

一句话收尾：**论文把它吹成了物理学，工程把它做成了可靠的机械——留下物理学的部分，相信机械的部分，删掉两者之间互相矛盾的说辞，这套东西就有了长期价值。**

---

## 附录 A：证据索引

| 主张 | 证据位置 |
|---|---|
| Cordis LIFO 恢复 | `cordis/packages/core/src/fiber.ts:282-293`（`disposables.splice(0).reverse()`） |
| Cordis 边界/acquisition/emission | Cordis PDF §6.1，pp.67-68 |
| Cordis 依赖环分析 | Cordis PDF §6.5，p.71 |
| dsh 万物皆插件 | `deepseek-harness/README.md` 开篇 |
| dsh append-only 投影 | `deepseek-harness/docs/architecture.md:45,94-96` |
| dsh vendored cordis + 18 处加固 | `deepseek-harness/vendor/README.md` |
| tokmon-n WAL/哈希链 | `tokmon-n/nyxia/storage/photon_store.cpp:117,289`；tests/core_tests.cpp |
| tokmon-n reconcile 五阶段 | `tokmon-n/nyxia/runtime/runtime.cpp:600-708` |
| tokmon-n afterglow 2s deadline | `tokmon-n/nyxia/runtime/runtime.cpp:708-732` |
| tokmon-n 零残留降格承诺 | `tokmon-n/docs/design-rs.md:89,2397,2604` |
| 测试全绿 | `tokmon-n/build/windows-msvc-release/Testing/Temporary/LastTest.log`（2026-08-25） |
| 论文硬编码模型响应 | RCLD 论文第 6 章 `model_response = "CALL_TOOL:calculate:128 * 4"` |
| C++20 基线 | `tokmon-n/CMakeLists.txt`（`CMAKE_CXX_STANDARD 20`） |
| Slint 而非 Skia | `tokmon-n/apps/tokmon-desktop/`（14 个 .slint）+ `.deps/slint` |

## 附录 B：方法与局限

- 本报告基于静态阅读 + 子代理全仓调研 + 少量本机命令验证（git log、ctest 日志、关键源码片段抽读）；未执行 clean rebuild、未运行 fuzz/压力测试、未进行真实 LLM 幻觉率对照实验；
- cordis 与 deepseek-harness 本地未安装完整 Node 依赖，未运行其测试套件；
- 评分为主观审查量表，用于传达相对判断，不具备度量学效力。
