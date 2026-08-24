# Tokmon 新版实现与验证报告

## 斜杠命令与双端交互补充（2026-08-23）

- 新增 33 条必要的 Tokmon 原生命令及统一 C++20 目录、解析、别名、引用参数和模糊匹配。
- CLI 与 Desktop 都把命令发送为 Snow `command.execute` 意图；客户端不写 Photon。
- 每次执行追加 `command.invoked`、Snow Lens 的 `command.observed`、`command.completed` 或 `command.failed`。
- Textus、Cove、Aya、Enso 分别承担压缩、Git/制品、子光线和技能发现；规划与审查进入真实智能体光路。
- Desktop 新增斜杠自动匹配悬浮层、点击选择回填、命令结果投影、设置弹窗联动、剪贴板和退出联动。
- `/rewind` 实现为从历史序号创建新 Ray，明确记录 `history_deleted: false`，没有撤销、编辑或删除接口。
- 详细用法见 [SLASH-COMMANDS.md](./SLASH-COMMANDS.md)。

> 架构题记：**A Lens to Them All**  
> 语言基线：C++20（未使用 C++23）  
> 报告日期：2026-08-23  
> 设计基线：[DESIGN.md](./DESIGN.md)  
> 内置透镜详设：[BUILTIN-LENSES-DESIGN.md](./BUILTIN-LENSES-DESIGN.md)
> UI、进程生命周期与端到端验收：[UI-DESKTOP-IMPLEMENTATION-REPORT.md](./UI-DESKTOP-IMPLEMENTATION-REPORT.md)

## 1. 本次交付结论

`tokmon-n/` 已形成一套可编译、可安装、可执行测试的新版 Tokmon。Nyxia 只保留微内核职责；业务能力以 `committed Photon → Lens view → Surface/Act → Beam refract → new Photon` 进入同一条光路。十九个正式内置透镜与 Calculator 参考透镜均有独立 C++ 实现、独立 `lens.yaml`、独立动态库和契约测试。

Windows 当前目标构建已完成：

- 核心、透镜、Snow 与生命周期测试：当前 `84/84` 通过；
- Slint 桌面目标：编译、链接成功；
- 安装冒烟：成功，安装树共 81 个文件；
- 动态透镜：20 个 DLL 全部生成；
- Node.js 和 CPython 透镜：真实 Worker 进程端到端通过；
- Rust：本次构建不需要，也没有要求用户补装。

## 2. 不可破坏的实现约束

### 2.1 只有一种业务语义

实现中没有引入第二套业务扩展机制。内置 C++、C ABI、native worker、Node.js 和 CPython 最终都适配成同一个 `ILens` 契约：

1. Lens 只读取不可变 `PhotonWindow`；
2. `view` 只贡献 Surface 或提出 Act，不执行现实 I/O；
3. 现实 I/O 只能在取得 Beam 后的 `refract` 中发生；
4. 结果必须追加为新 Photon，不能回写旧事实；
5. 跨透镜协作依赖 Photon、SurfaceChannel 和 ActPattern，不依赖彼此的 C++ 对象。

### 2.2 因果光子流物理不可改写

`PhotonStore` 使用 SQLite WAL 和单写入口。提交时生成全局单调 sequence、previous hash、canonical CBOR payload hash 和整条 SHA-256 hash chain。数据库 trigger 直接拒绝 committed Photon 的 `UPDATE` 与 `DELETE`。

取消、失败、补偿、重试、恢复和回滚均追加新 Photon：

- 取消追加 `ray.cancelled` 或相应 cancel observation；
- 失败追加 `act.failed`，外部结果不确定时追加 outcome-unknown；
- workflow 补偿产生新 Act 和新 attempt；
- 回到旧透镜 artifact 必须发布一个更高 mount epoch；
- 新会话只清空 Termon projection cache，不删除 daemon 历史。

## 3. 运行时组合与热替换

`TokmonRuntime::reconcile()` 同时读取用户级 `~/.tokmon/` 和项目级 `<workspace>/.tokmon/` YAML。候选光路在 dark lane 中完成以下验证后才可发布：

- manifest、ABI、Lens ID、runtime kind 和 artifact hash；
- `lens-lock.yaml`、schema/SBOM evidence hash 和可选 HMAC-SHA256 签名；
- 依赖、冲突、optical order、重复 Lens ID 和精确 ActPattern 冲突；
- 权限是否相对旧 generation 扩张；
- 空 PhotonWindow 的真实 `view` 调用；
- Worker hello/ready、runtime 和 Lens ID 握手。

成功后先 durable append `mount.epoch-committed`，再原子发布不可变 `LightPathSnapshot`。新 Beam 只进入新 generation；旧 generation 进入 afterglow，停止接收新 Beam，等待或取消在途 Beam，然后关闭 watcher、socket、Worker、子进程和动态库。

组合管理可通过 CLI/Snow 执行：

```text
tokmon lens list
tokmon lens verify
tokmon lens mount <id> <artifact> [runtime]
tokmon lens replace <id> <artifact> [runtime]
tokmon lens unmount <id>
tokmon lens reconcile
```

配置写入采用临时文件 + 原子替换。卸载和回滚只改变下一 epoch 的光路，不触碰历史 Photon。

## 4. 二十个内置透镜的实现

| 透镜 | 主要已实现能力 | 现实边界与专项证据 |
| --- | --- | --- |
| Ignis | manifest/lock/signature/SBOM/schema evidence、依赖与冲突、dark lane、desired/current diff、mount/replace/unmount/reconcile、generation 换代 | C ABI 换代测试验证更高 epoch 原子发布；未知 YAML 不发布候选路径 |
| Lemon | typed frame、容量/单帧/批量上限、单消费者/消费组/广播、producer tail、consumer cursor、背压、gap、durable cursor、epoch bridge | cursor 倒退、越界 frame 和 afterglow 由契约测试覆盖 |
| Iris | MCP client、stdio/HTTP JSON-RPC、工具/资源/提示 catalog、真实健康探测、latency/capability hash、重连观察；LSP initialize/initialized/didOpen/request/shutdown/exit 与结果归一化；MCP server 以受管 server Act 接入 | Python MCP fixture 的真实发现/调用；Iris catalog → Techor → Iris 组合链；真实 LSP hover 生命周期 |
| Rhea | OpenAI、Anthropic、DeepSeek、Gemini 与 local deterministic adapter；SSE/JSON 解析；reasoning/content/tool-call/usage 分流；timeout、Retry-After、指数退避、确定性 jitter、fallback broker、usage/cost、响应 hash、Cista binding | 本地 HTTP fixture 验证 503 重试、reasoning 隔离、content streaming 和 usage |
| Janus | 从 Photon 重建 turn/step；need-model、tool result 后续拍、预算、重复 Act/无进展检测、steer/cancel/stop、状态显像 | Calculator golden ray 完成多拍模型—工具—模型闭环，并验证同一 ray 多轮追加 |
| Clotho | YAML DAG、依赖/环/权限/schema 验证、模板、条件、fan-out/fan-in、join、全局/分组并发、retry attempt、timeout 元数据、continue/stop/compensate、pause/resume/cancel | DAG、确定性 ready、fan-out、group limit、显式 compensation 专项测试 |
| Aya | fork/spawn、独立 child ray、预算/deadline/ActKind 上界、受控引用继承、禁止 secret 继承、只读 workspace、真实 Git worktree、parent/child/sibling message、progress/help/heartbeat/usage、取消传播、all/any/quorum/manual join、摘要/artifact/conflict、Cove merge proposal | 真实 Git 仓库创建隔离 worktree；进度/心跳/usage 只从 Photon 折叠 |
| Textus | system/instruction/conversation/tool/memory/RAG 稳定组装、保守 token estimator、来源预算、最新输入与未完成调用保留、去重/相关性/滑窗/摘要/截断/溢出、reasoning 分区、稳定 cache key | token 预算和确定性由透镜契约覆盖 |
| Enso | 用户/项目/artifact SKILL 发现、渐进加载、根内引用链/hash；append-only memory proposal/accept/reject/supersedes；文件/artifact 摄取、chunk、embedding、关键词+向量混合检索、过滤/重排、增量 tombstone | 真实 SKILL.md、RAG revision 与 memory provenance 专项测试 |
| Techor | LightPath/MCP/worker/内置/Code Mode 统一工具目录；严格 JSON schema、schema drift、unknown/ambiguous tool、稳定幂等路由；受限 `tokmon-act-v1` Code Mode | MCP catalog 真实组合调用；Code Mode 编译为结构化 Act，不能直接 I/O |
| Styx | `SandboxPlan`、argv 无 shell 执行、cwd/env allowlist、文件/网络范围、CPU/Mem/PID/output/deadline、bounded ring、PTY stdin/stdout/resize/exit、cooperative cancel 后进程树终止、Wasmtime CLI 与 Docker CLI adapter | Windows Job Object、ConPTY 和真实取消测试；adapter 缺失时 fail closed，禁止退化成本机裸执行 |
| Fallen | deny → allow → ask、用户根策略限制项目策略、risk/trust/path/argv/参数/时间匹配、多级审批、Act hash/epoch/generation/deadline 绑定、one-shot/session、内容分类 | `approved=true` 不能绕过公共 admission；不可信文本分类不回显 secret |
| Cista | Windows Credential Manager create/read/rotate/delete/list metadata；exact Act + target generation + purpose + epoch + lifetime 的一次性 binding；schema-aware redaction 与内存清零 | 真实 OS credential 读写与一次消费测试；Photon 中无明文 |
| Chora | Photon WAL/append gate 显像、不可变版本 KV、内容寻址 Blob、去重/校验、Windows DPAPI 敏感 Blob、checkpoint、archive、backup manifest/restore | 真实 blob 写入、地址 hash 和不可变性测试 |
| Tracket | sequence/id/parent/epoch/schema/hash/caused-by-act 验证、timeline/causal DAG、raw trace vault、R0/R1/R2/R3 replay、fork/export/audit/integrity report | 断链与未知 required schema 进入不可信诊断，不伪造可信 replay |
| Nota | chLog 结构化事件、correlation、Span/metric fold、OTLP HTTP exporter、Prometheus 文本和真实 loopback `/metrics` server、运行时 filter/sample、health/doctor/diagnostic bundle | 真实 HTTP collector 与 Prometheus GET 专项测试；endpoint 只允许 loopback |
| Cove | canonical entity tree、类型/大小/mtime/hash/Git/ignored；create/modify/delete/rename 聚合；branch/HEAD/index/worktree/untracked/conflict；pre/post image、guarded read/write/create/move/delete、Git evidence、artifact/provenance | 真实文件生命周期、Git repo/ignored 状态、polling watcher 与 worktree 测试 |
| Snow | interactive chat、headless run、history、JSONL/CBOR、stdio 并发、request id、stream/cancel/ordered close、named pipe/Unix socket、snapshot/cursor delta/gap/reconnect/idempotency、deadline、doctor、Lens 管理 | 真实本地 transport 并发、stdio 顺序关闭、断线 cursor 恢复测试 |
| Termon | 13 个 `ui.*` SurfaceChannel：conversation、trajectory、code、terminal、approval、context、models、tools、workspace、children、lenses、diagnostics、settings；Slint 仅保存 projection cache | tokmond 新增按 ray 折叠 Surface；桌面端聊天后读取 `ui.trajectory`，所有提交仍由 daemon 完成 |
| Calculator | C++20 开发者参考透镜，严格表达式校验、确定性 Act 与 `tool.result` | 完整 Fact → Lens → Act → Photon golden ray |

## 5. C++、Node.js 与 CPython 透镜

三类扩展最终都遵守相同 Lens 契约：

| 形式 | 进程/ABI 边界 | 热替换单位 |
| --- | --- | --- |
| C++ 动态透镜 | `tokmon_lens_entry_v1` 稳定 C ABI | DLL/shared-object generation |
| JS/编译后的 TS | Node.js ESM adapter + Worker Protocol | 独立 Worker generation |
| Python | CPython adapter + Worker Protocol | 独立 Worker generation |

脚本透镜没有使用 QuickJS 或 MicroPython，因此可以使用正常的 npm/PyPI 包生态。依赖必须在构建/打包时锁定进不可变 artifact；运行时不在线安装包，也不允许联网改变依赖图。

Worker Protocol 使用 4-byte 大端长度 + canonical CBOR frame。C++ host 会复核脚本返回的 SurfaceChannel、Act proposal、Photon kind 与 manifest 权限；Worker 异常、超时或退出被转换成 `tl::expected<..., Error>`，不能穿透 Nyxia。

## 6. Snow、daemon 与桌面进程

- `tokmond`：唯一持有 `TokmonRuntime` 和 Photon append gate 的 daemon；
- `tokmon`：Snow CLI 客户端，不拥有事实状态；
- `tokmon-desktop`：Slint/Termon projection 客户端；
- `tokmon-launcher`：仅作为可选兼容快捷入口，不是 CLI 或桌面的前置进程；
- `tokmon-lens-worker`：受管 C ABI、Node.js、CPython Lens 进程边界。

会话支持同一 ray 的多轮追加。CLI `/new` 和桌面“新会话”只让下一输入创建新 ray。Snow chat 支持 `deadline_ms`；deadline 到达时取消 ray，在途进程先 cooperative stop，再终止进程树。

`tokmon` 与 `tokmon-desktop` 都会先探测按工作区隔离的 Snow endpoint；不存在时由当前可执行文件定位同目录 `tokmond`，以后台无窗口方式拉起并等待 ready。客户端通过心跳租约声明存活：Desktop 与交互 CLI 退出后安全空闲停止，一次性 CLI 保留 15 秒复用窗口；显式 `tokmon daemon start` 会 pin 常驻，直到 `tokmon daemon stop`。daemon 只在没有其他租约且 Nyxia 没有活动工作时自动退出，并通过 Windows named mutex 或 POSIX `flock` 拒绝同 endpoint 的第二实例。详见 `docs/DAEMON-LIFECYCLE.md`。

Desktop 导航节点现在具有工作空间语义：项目保存规范化绝对路径，会话默认继承父项目并可显式覆盖。选择节点会在运行期完成目标 tokmond/endpoint 租约交接，再新建空 Ray 或恢复该工作空间中的已有 Ray。导航树始终由启动 Desktop 时的导航 workspace 保存，因此活动项目切换不会分裂树配置；Photon、模型配置和文件工具仍按活动 workspace 隔离。

Termon 不再用硬编码 timeline/code model 作为事实来源。桌面端先消费 snapshot/cursor delta；发送 chat 后再请求该 ray 的 `SurfaceSnapshot`，从 Termon 的 `ui.trajectory` 重建活动会话投影。

## 7. 配置、错误和日志

- 配置格式：YAML；
- 用户级目录：`~/.tokmon/`；
- 项目级目录：`<workspace>/.tokmon/`；
- 可预期失败：`tl::expected<T, tokmon::Error>`；
- C++ 日志：chLog；
- 语言标准：C++20。

YAML loader 拒绝未知字段、未知 runtime、非法资源上限、缺失 entry、错误 ABI、重复依赖和越界路径。日志、Error、Photon、Surface、HTTP 错误正文和 diagnostic bundle 在进入 sink 前执行 redaction。

## 8. 编译、测试和安装结果

### 8.1 核心构建和全量测试

```powershell
cmake --build build/windows-msvc-ui-debug --target tokmond tokmon tokmon-desktop tokmon-tests -j 4
ctest --test-dir build/windows-msvc-ui-debug --output-on-failure -C Debug
```

当前结果：`84/84` 通过，`0` 失败；最新真实 OpenCode 与 Desktop 原生 UI 验收见 [OPENCODE-DESKTOP-ACCEPTANCE-REPORT.md](OPENCODE-DESKTOP-ACCEPTANCE-REPORT.md)。

测试包含：

- canonical CBOR/JSON bridge；
- SQLite 物理 append-only 与 hash chain；
- LightPath 原子 epoch、artifact signature 和 C ABI hot swap；
- 二十个动态库身份、二十镜声明折射场景和逐镜契约；
- Node.js/CPython SDK 与真实 WorkerLensProxy；
- 真实 MCP、LSP、HTTP provider、OTLP、Prometheus；
- 真实进程、ConPTY、取消、Git、worktree、文件 watcher；
- DAG、RAG、SKILL、memory、Cista、Chora；
- Snow 并发、stdio、cursor 重连；
- Termon 十三个页面 SurfaceChannel。

### 8.2 带 Slint 的 Windows 构建

```powershell
cmake --build build/windows-msvc-ui-debug --parallel 4
```

结果：成功生成 5 个应用程序、20 个 `tokmon-lens-*.dll` 和 `slint_cpp.dll`。编译器只有第三方库 warning，没有 Tokmon 编译或链接错误。

### 8.3 安装冒烟

```powershell
cmake --install build/windows-msvc-ui-debug --prefix build/install-smoke
```

结果：成功。安装树含 81 个文件，包括应用程序、20 个透镜 DLL、Slint runtime、40 个 Figma SVG、默认 YAML、TypeScript adapter 和 Python adapter。

### 8.4 实际工具链

| 工具 | 本机结果 |
| --- | --- |
| CMake | 4.2.1 |
| Clang/LLVM | 17.0.6 |
| Node.js | v25.8.1 |
| CPython | 3.10.7 |
| Git | 2.53.0.windows.2 |
| Slint C++ runtime | 项目构建目标成功 |

## 9. 环境受限但不会静默降级的验证项

以下是外部执行后端的环境状态，不应伪装成已完成现场验证：

- Wasmtime CLI 本机未安装。Styx 的 WASI adapter、参数校验、scope 和 fail-closed 路径已实现；本机没有执行真实 `.wasm` 模块。
- Docker CLI 29.4.0 已安装，但 Docker Desktop daemon 未运行。容器 create/copy/exec/stop/remove 生命周期已实现；本机没有完成 live container 测试。
- 当前完整编译与现实系统测试在 Windows 上执行。Cista 当前使用 Windows Credential Manager；非 Windows credential backend 保持明确 `unsupported`，不会退化成明文文件。
- Chora 敏感 Blob 在 Windows 使用 DPAPI；非 Windows 未配置 envelope backend 时明确拒绝敏感写入。

这些限制不影响本次 Windows 目标的 84 项测试与 Slint 构建，但若把“完成”定义为所有 OS/所有可选外部后端都做现场验收，则仍需相应平台或 daemon 环境。由于这些后端会 fail closed，缺少环境不会导致较弱隔离被冒充为成功。

## 10. 关键文件索引

- 微内核与换代：`nyxia/runtime/runtime.cpp`
- 光流发动机：`nyxia/engine/ray_tracing_engine.cpp`
- append-only storage：`nyxia/storage/photon_store.cpp`
- manifest/lock：`nyxia/loader/manifest_io.cpp`
- C ABI：`nyxia/loader/c_abi_loader.cpp`
- Worker：`nyxia/worker/worker_lens_proxy.cpp`
- Snow transport：`protocol/snow_transport.cpp`
- daemon：`apps/tokmond/main.cpp`
- CLI：`apps/tokmon-cli/main.cpp`
- Slint host：`apps/tokmon-desktop/main.cpp`
- Slint UI：`apps/tokmon-desktop/ui/tokmon.slint`
- 二十镜源码：`lenses/*/*_lens.cpp`
- 核心与现实边界测试：`tests/unit/core_tests.cpp`、`tests/unit/builtin_lens_tests.cpp`
- 逐镜契约：`lenses/*/tests/*_contract.cpp`

## 11. Rust 工具链结论

当前构建使用已经可用的 Slint C++ 依赖，`tokmon-desktop.exe` 已成功生成，因此不需要手工补齐 Rust/Cargo。只有未来决定从 Slint Rust 源码重新构建其 C++ SDK，或主动引入必须由 Cargo 构建的组件时，才需要安装 Rust 工具链。
