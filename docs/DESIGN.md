# Tokmon：A Lens to Them All

## C++20 + Slint 总体设计与实现

> 文档状态：Architecture Baseline / Implementation Guide 3.0  
> 设计基线：2026-08-22  
> 目标目录：`tokmon-n/`  
> 宿主实现语言：ISO C++20  
> 动态 Lens 开发语言：C++20、JavaScript/TypeScript（Node.js）、Python（CPython）与 WASM  
> 桌面显像：[`Slint`](https://github.com/slint-ui/slint)  
> 唯一闭环：**Fact → Lens → Act → new Fact**

本文重新定义新 Tokmon 的架构、实现边界、动态透镜 SDK、二十透镜职责、光流发动机、Slint 桌面端、数据格式、安全管线、测试方法与迁移路线。

规范来源按以下优先级排列：

1. 本文；
2. [`tokmon-lens-architecture-explained.zh.md`](tokmon-lens-architecture-explained.zh.md) 中的透镜公理、6.5 光流发动机和第 7 章动态透镜开发模型；
3. 架构论文中文版、英文版及 `advise.md` 中与纯透镜语义一致的部分；
4. 旧 C++ Tokmon 仅作为产品需求、协议、安全经验和 UI 资产来源。

其他理论资料只能作为历史启发，不得把它们的术语、抽象对象或执行模型引入本设计。Tokmon 的公开语义、内部语义和实现命名必须全部来自透镜、光子、光流、光路、视界、折射和显像。

---

## 目录

1. [结论先行](#1-结论先行)
2. [目标与非目标](#2-目标与非目标)
3. [透镜原生概念体系](#3-透镜原生概念体系)
4. [总体光路架构](#4-总体光路架构)
5. [Fact Plane：不可改写的因果光子流](#5-fact-plane不可改写的因果光子流)
6. [Nyxia：原初棱镜微内核](#6-nyxia原初棱镜微内核)
7. [双向透镜编程模型](#7-双向透镜编程模型)
8. [开发者实战：5 分钟编写动态透镜](#8-开发者实战5-分钟编写动态透镜)
9. [动态装卸与运行时替换](#9-动态装卸与运行时替换)
10. [RayTracingEngine：光流发动机](#10-raytracingengine光流发动机)
11. [二十透镜总览](#11-二十透镜总览)
12. [二十透镜详细设计](#12-二十透镜详细设计)
13. [端到端光流](#13-端到端光流)
14. [Act Plane：现实动作管线](#14-act-plane现实动作管线)
15. [本地协议、配置与目录](#15-本地协议配置与目录)
16. [Termon：Slint 桌面显像](#16-termonslint-桌面显像)
17. [C++20 工程与构建](#17-c20-工程与构建)
18. [安全与供应链](#18-安全与供应链)
19. [诊断、性能与容量](#19-诊断性能与容量)
20. [测试策略](#20-测试策略)
21. [旧版迁移](#21-旧版迁移)
22. [实现路线图](#22-实现路线图)
23. [验收清单](#23-验收清单)
24. [关键决策与风险](#24-关键决策与风险)
25. [参考](#25-参考)

---

## 1. 结论先行

新 Tokmon 是一个由一条不可改写的因果光子流、一个原初棱镜微内核、一台按拍推进的光流发动机和二十个双向透镜构成的本地 Agent 系统。

系统必须同时满足以下不变量：

1. **光子只能追加**：committed Photon 不能编辑、覆盖、删除、替换、撤销或复用序号；
2. **透镜只有两种行为**：`view` 从光流投影视界，`refract` 接收 Act 并把执行结果折射成新光子；
3. **只有一条直线光路**：透镜之间不得直接调用、私建总线或横向广播；
4. **按需拉取显像**：Prompt、UI、CLI、诊断画面都在需要时从 Fact 重新折叠，不成为第二事实源；
5. **无新 Act 即自然停机**：一拍结束后既无新 Act、也无等待中的现实结果，光流发动机立即静止；
6. **Nyxia 只守光学定律**：Nyxia 是唯一静态内建的元框架微内核，不承载模型、工具、存储、策略或 UI 业务；
7. **其余十九透镜可换代**：运行中的新代码必须能在不重启整个 daemon 的情况下接管后续光束；
8. **现实动作只有一条出口**：所有 Act 都经过 Techor、Fallen、Cista、Styx、目标透镜和 Chora/Tracket 的固定管线；
9. **Lens 不拥有规范状态**：可丢弃缓存、连接和工作线程由 Nyxia 的镜座托管；拔出镜片后，其后续显像贡献和新 Act 接收资格立即归零；
10. **全工程只使用 C++20**：源码、依赖、CI、示例和动态边界都以同一语言基线实现。

```text
                    append-only Causal Photon Stream
                                  │
                                  ▼
                 ┌────────────────────────────────┐
                 │ Nyxia + Lemon + RayTracingEngine│
                 │   pull → fold → extract → act   │
                 └────────────────┬───────────────┘
                                  │ ordered LightPath
                                  ▼
     Ignis → Iris → Textus → Enso → Rhea → Janus → Techor → Fallen
                                  │
                                  ▼
                      Cista → Styx → Cove / target
                                  │
                                  ▼
                       new Photon → Chora append
                                  │
                  ┌───────────────┴───────────────┐
                  ▼                               ▼
              Snow Surface                  Termon Surface
                                               Slint
```

图中顺序表达一条典型 Agent 光路，不等于每一拍必须让每个透镜执行现实动作。`view` 是线性折叠；`refract` 只在 Act 的光斑模式命中时发生。

---

## 2. 目标与非目标

### 2.1 目标

- 重写旧 C++ Tokmon，保留成熟的产品流程、安全经验、工作区能力和 UI 需求；
- 把二十个命名模块统一成同一种双向透镜开发模型；
- 给出可以直接落地的 C++20 SDK、C ABI、worker 协议和示例；
- 让 Prompt、tool schema、UI、CLI、诊断与历史回放成为光流的即时投影；
- 让动态透镜的加入、换代和拔出具有可测试的零新贡献语义；
- 让崩溃恢复只依赖 committed Photon 和不可变光路版本；
- 使用 Slint 构建 Windows、macOS、Linux 原生桌面端；
- 从第一阶段建立 golden ray、崩溃点矩阵、属性测试、ABI 测试和 UI 截图测试。

### 2.2 非目标

- 不建立 EventBus、PubSub、全局 signal 网或任意字符串 topic；
- 不允许透镜保存会改变恢复结果的私有规范状态；
- 不把最终 Prompt、UI widget tree、窗口临时状态、日志或遥测当作恢复源；
- 不把每个函数、token 或控件都拆成独立透镜；
- 不允许透镜绕过 `PhotonEmitter` 修改历史；
- 不允许透镜绕过 `ActGate` 直接执行外部写操作；
- 不在同进程加载来源未知的 native code；
- 不移植旧 White 的 DOM/CSS/Lexbor/Yoga/Skia/SDL3 UI 栈；
- 不把未经本地 benchmark 验证的纳秒、内存或帧率数字写成产品承诺；
- 不引入第二套组合理论、生命周期理论或状态语义；
- 不跟随 Slint 的移动分支，发布版本始终固定精确 tag 和校验值。

---

## 3. 透镜原生概念体系

### 3.1 六个基础对象

#### Fact / Photon

Fact 是已经发生且被 Tokmon 承认的事实。Photon 是 Fact 在因果光流中的物理封装。本文在谈语义时使用 Fact，在谈存储、传输和匹配时使用 Photon。

每个 committed Photon 至少拥有：全局序号、光流标识、因果父光子、来源透镜、种类、schema、载荷哈希、时间、光路版本和完整性校验。

#### CausalRay

`CausalRay` 是按序追加的 Photon 集合。它可以 fork 出新光流，但旧光流永不被修改。所谓“回到过去”只能以某个旧光子为父节点创建新光流。

#### Lens

Lens 是双向镜片：

```text
view    : PhotonWindow × SurfaceBuilder → SurfaceBuilder
refract : PhotonWindow × Act × PhotonEmitter → RefractionResult
```

- `view` 只观察光子并贡献视界，不执行外部写操作；
- `refract` 只在 Act 模式命中且 Act 已通过固定管线后运行；
- `refract` 不能取得可变光流，只能通过只追加 emitter 产生新 Photon；
- Lens 可以真实计算、读写受控资源和调用外部系统，但结果必须以 Photon 回到光流。

#### Surface

Surface 是某一刻从光流折叠得到的视界。主要种类：

| Surface | 用途 | 最终消费者 |
| --- | --- | --- |
| `ModelSurface` | system instruction、messages、tools、预算 | Rhea |
| `UiSurface` | 会话、计划、审批、Diff、终端、完成状态 | Termon |
| `CliSurface` | 命令行输出、机器可读结果 | Snow |
| `ActSurface` | 候选 Act、风险标记、审批状态 | Techor/Fallen |
| `DiagnosticSurface` | 光路、镜座、积压、失败和版本 | Nota/doctor |

Surface 是可丢弃投影。缓存丢失后必须能从 Photon 与当前 LightPath 重建。

#### Act

Act 是准备触碰现实世界的结构化动作。它不是 Photon，也不能伪装成已发生的事实。Act 至少包含：

```cpp
struct Act {
    ActId id;
    std::string kind;          // tool.calculate, fs.write, process.exec, model.call ...
    LensId target;
    SchemaId arguments_schema;
    Bytes arguments;
    ImpactClass impact;
    PhotonId caused_by;
    MountEpoch light_path_epoch;
    ApprovalMode approval;
    IdempotencyKey idempotency_key;
};
```

Act 成功、失败、被拒绝、超时或结果未知，都会产生新的 Photon。

#### LightPath

LightPath 是当前按顺序装入的 Lens generation 的不可变数组。发动机每一拍只读取一次 LightPath 快照，因此一拍内部顺序确定；换代只影响后续新拍和未开始的 Act。

### 3.2 三条透镜公理

1. **唯光不灭**：过去只存在于只追加因果光子流中；
2. **万物皆透镜**：业务能力只能通过 `view/refract` 加入系统；
3. **组合即叠镜**：挂载改变后续光束经过的镜片顺序，拔出让该镜片对后续光束立即失去贡献资格。

### 3.3 七条 Lens Law

1. **Append Only**：只有 Chora 的 append gate 能产生 committed Photon；
2. **Pure View**：相同 PhotonWindow、LightPath 版本和配置必须得到相同视界贡献；
3. **Act Before Reality**：现实执行前先提交 Act 意图和准入结果；
4. **No Side Channel**：Lens 之间禁止直接调用和横向通信；
5. **Linear Fold**：所有显像贡献按 LightPath 顺序折叠；
6. **Patterned Refraction**：只有 manifest 声明并通过 schema 校验的 Act 才能命中 `refract`；
7. **Darkness Means Stop**：一拍没有产生新 Act 且没有待收束光束时，发动机停止。

### 3.4 “无状态”的工程含义

Lens 无状态不是说进程不能有缓存或 socket，而是说：

- Lens 不能把恢复所需的唯一数据藏在对象字段中；
- 派生缓存必须带 `last_photon_seq + light_path_epoch`，随时可丢弃重建；
- 网络连接、线程、timer、watcher、子进程和临时文件必须通过镜座提供的 `OpticalHost` 创建；
- 镜片拔出后，镜座能统一停止这些活动；
- 任何已经发生的外部结果仍以 Photon 保存，拔镜不能抹掉历史或现实。

---

## 4. 总体光路架构

### 4.1 三个平面

```text
┌───────────────────────────────────────────────────────────────┐
│ Fact Plane                                                    │
│ PhotonEnvelope · CausalRay · Chora · Tracket · immutable blob │
└──────────────────────────────┬────────────────────────────────┘
                               │ PhotonWindow
┌──────────────────────────────▼────────────────────────────────┐
│ Lens Plane                                                    │
│ Nyxia · LightPath · Lemon · RayTracingEngine · view/refract   │
└──────────────────────────────┬────────────────────────────────┘
                               │ Act
┌──────────────────────────────▼────────────────────────────────┐
│ Act Plane                                                     │
│ Techor → Fallen → Cista → Styx → target → PhotonEmitter       │
└───────────────────────────────────────────────────────────────┘
```

- Fact Plane 回答“发生过什么”；
- Lens Plane 回答“现在如何看、下一步能做什么”；
- Act Plane 回答“如何受控地触碰现实”。

### 4.2 五条主光路

#### 输入光路

```text
Termon/Snow/Iris source
→ normalize
→ PhotonDraft
→ Tracket validate
→ Cista redact
→ Chora append
→ committed Photon
```

#### 模型光路

```text
PhotonWindow
→ ordered view fold
→ ModelSurface
→ model.call Act
→ Fallen/Cista admission
→ Rhea.refract
→ model.* Photons
```

#### 工具光路

```text
model.response Photon
→ Techor pattern extraction
→ typed Act
→ Fallen → Cista → Styx
→ target Lens.refract
→ tool.* Photons
```

#### 显像光路

```text
PhotonWindow
→ ordered view fold
→ UiSurface / CliSurface
→ Termon / Snow
```

#### 换镜光路

```text
desired light-path Photon
→ Ignis validate candidate
→ dark-lane replay
→ mount.commit Act
→ new mount-epoch Photon
→ Nyxia atomic LightPath swap
→ old generation afterglow drain
```

### 4.3 进程布局

```text
tokmon-launcher
├─ verifies signed bootstrap and starts tokmond
├─ starts/restarts tokmon-desktop
└─ performs desktop handoff during Termon replacement

tokmond
├─ Nyxia static microkernel
├─ RayTracingEngine
├─ trusted native lens generations
├─ worker manager
├─ Chora single append writer
└─ Snow local protocol endpoint

tokmon-lens-worker
├─ one trust group per process
├─ restricted OpticalHost bridge
├─ native / Node.js / CPython runtime adapter
└─ crash isolation for dynamic lenses

tokmon-desktop
├─ Termon controller
├─ Slint event loop
└─ reconnectable Snow client
```

`tokmon-launcher` 只是物理启动入口，不是额外透镜。daemon 中只有 Nyxia 能发布新 LightPath。

### 4.4 线程规则

- Slint event loop 固定在 desktop 主线程；
- tokmond 的 Photon append writer 单线程串行分配 `seq`；
- 每个 active ray 同一时刻只有一个 engine step 能提交状态；
- 模型流、PTY、watcher 和 worker I/O 使用有界队列；
- `view` 默认在只读折叠执行器上运行，不允许阻塞 I/O；
- `refract` 通过 C++20 coroutine、`std::jthread` 和 `std::stop_token` 执行；
- 所有跨线程 UI 更新通过 Slint event-loop queue 批量投递；
- 禁止 `detach()`、无限队列和未归属镜座的后台循环。

### 4.5 启动闭环

Nyxia 不把十九个业务透镜静态编进内核。签名保护的 `bootstrap.lock.yaml` 只描述启动所需的 artifact：Ignis、Lemon、Chora、Tracket、Cista、Fallen、Snow。

```text
launcher verifies tokmond + bootstrap.lock.yaml
→ Nyxia starts with empty LightPath
→ load and verify bootstrap lens artifacts
→ open Chora append gate
→ Tracket verifies tail hash and last mount epoch
→ reconstruct desired LightPath from photons
→ Ignis validates remaining generations
→ Nyxia publishes immutable LightPath snapshot
→ RayTracingEngine accepts new rays
→ Termon connects through Snow
```

任何一步失败都进入只读 rescue 显像：可以检查、导出和修复，但不能假装系统已经正常接收 Act。

---

## 5. Fact Plane：不可改写的因果光子流

### 5.1 PhotonEnvelope

```cpp
struct PhotonEnvelope {
    PhotonId id;                       // UUIDv7/128-bit sortable id
    StreamId stream_id;
    std::uint64_t seq;                 // stream 内严格递增
    std::vector<PhotonId> parents;     // 单父或 merge 多父
    LensId origin_lens;
    std::string kind;                  // user.input, model.chunk, tool.result ...
    SchemaId schema;
    MountEpoch light_path_epoch;
    std::chrono::sys_time<std::chrono::nanoseconds> observed_at;
    std::optional<std::chrono::sys_time<std::chrono::nanoseconds>> source_at;
    BlobRef payload;
    Hash payload_hash;
    Hash previous_hash;
    Hash envelope_hash;
    TraceId trace_id;
};
```

`observed_at` 是 Tokmon 承认该 Fact 的时间；`source_at` 是外部来源自报时间，不能替代提交顺序。

Envelope 不存在可变 `status`、`revision`、`deleted` 或 `revoked` 字段。认识错误时追加 `*.corrected`，现实需要修复时提出补偿 Act，当前视界不再采用旧信息时只改变 `view` 规则。

### 5.2 Photon family

| family | 示例 |
| --- | --- |
| input | `user.input`, `ui.intent`, `cli.intent`, `external.observed` |
| model | `model.call.requested`, `model.dispatched`, `model.chunk`, `model.message`, `model.usage`, `model.failed` |
| act | `act.proposed`, `act.admitted`, `act.denied`, `act.started`, `act.succeeded`, `act.failed`, `act.outcome-unknown` |
| tool | `tool.call`, `tool.result`, `tool.error` |
| workspace | `fs.observed`, `fs.changed`, `git.diff`, `artifact.created` |
| approval | `approval.requested`, `approval.decided`, `approval.expired` |
| ray | `ray.started`, `ray.step`, `ray.darkened`, `ray.cancelled`, `ray.failed` |
| lens | `lens.candidate`, `lens.mounted`, `lens.replaced`, `lens.unmounted`, `lens.rejected`, `lens.afterglow-ended` |
| system | `config.selected`, `doctor.report`, `recovery.performed` |

Schema 名称表达发生的事实，而不是可被原地修改的实体行。

### 5.3 提交协议

所有来源只能创建 `PhotonDraft`。固定提交路径为：

```text
PhotonDraft
→ Tracket: schema/causality/order validation
→ Cista: secret and sensitive-field redaction
→ Chora: payload put-if-absent + append transaction
→ committed PhotonEnvelope
→ Lemon cursor notification
```

关键规则：

- `seq` 由 Chora 单 writer 分配；
- payload 大于阈值时先写 immutable blob，再在同一事务引用；
- 只有事务成功后的 committed Photon 才能被 `view` 或 `refract` 观察；
- 通知可以重发，消费者按 `(stream_id, seq)` 去重；
- append 失败不得先执行现实动作；
- Act 结果无法确认时追加 `act.outcome-unknown`，不得伪造失败或成功。

### 5.4 SQLite v1

```sql
CREATE TABLE photons (
  stream_id         BLOB    NOT NULL,
  seq               INTEGER NOT NULL,
  photon_id         BLOB    NOT NULL UNIQUE,
  parents_cbor      BLOB    NOT NULL,
  origin_lens       TEXT    NOT NULL,
  kind              TEXT    NOT NULL,
  schema_id         TEXT    NOT NULL,
  light_path_epoch  INTEGER NOT NULL,
  observed_at_ns    INTEGER NOT NULL,
  source_at_ns      INTEGER,
  payload_ref       TEXT    NOT NULL,
  payload_hash      BLOB    NOT NULL,
  previous_hash     BLOB    NOT NULL,
  envelope_hash     BLOB    NOT NULL,
  trace_id          BLOB    NOT NULL,
  PRIMARY KEY (stream_id, seq)
) STRICT;

CREATE TRIGGER photons_no_update
BEFORE UPDATE ON photons
BEGIN
  SELECT RAISE(ABORT, 'committed photons are immutable');
END;

CREATE TRIGGER photons_no_delete
BEFORE DELETE ON photons
BEGIN
  SELECT RAISE(ABORT, 'committed photons are append-only');
END;
```

生产写连接隐藏在 Chora 私有进程边界。其他透镜只有 cursor read API，不能取得 SQLite 写句柄。

### 5.5 Fork、merge 与“回到过去”

```text
main:  P1 → P2 → P3 → P4
                    ╲
fork:                F1 → F2
```

- fork 追加 `ray.forked`，记录 base photon；
- fork 拥有新 `stream_id` 和独立 seq；
- merge 是新的 Cove Act，结果追加 merge Photon；
- 冲突、失败和放弃也追加记录；
- 任何操作都不删除 main 或 fork 的旧 Photon。

### 5.6 Checkpoint、归档与容量

Checkpoint 是加速 `view` 的派生索引，包含 reducer version、截至 seq、light path epoch、state hash 和 payload ref。它不是 Fact，可以丢弃重建。

长期数据分层：

- hot：最近 Photon envelope 和活跃 payload；
- warm：不可变压缩 segment；
- cold：内容寻址归档；
- checkpoint：可删除派生文件。

归档只移动不可变副本，不能从规范光流中抹去可寻址历史。保留策略必须通过增加新的密文轮换或访问限制实现，不能通过改写 Photon 实现。

---

## 6. Nyxia：原初棱镜微内核

### 6.1 唯一静态边界

Nyxia 静态链接进 `tokmond`。它只包含：

1. `RayTracingEngine` 驱动入口；
2. immutable `LightPathSnapshot` 的原子发布；
3. Lens artifact 验证、加载和 C ABI adapter；
4. `LensMount`、`MountGuard`、`BeamTicket` 与换代控制；
5. `OpticalHost` 的最小系统调用表；
6. 有界 Lemon 光纤与 cursor wakeup；
7. Photon append gate、Act gate 的不可绕过连接；
8. 动态代码的 worker/WASM 边界；
9. 停止、崩溃和诊断所需的最小状态；
10. 以 `tl::expected` 为唯一可恢复错误返回模型，以 `spdlog` 为统一日志出口。

Nyxia 不包含 provider 策略、Prompt、tool catalog、memory、workflow、workspace、存储业务、审批规则或 UI 页面。

### 6.2 光具座对象

```cpp
struct LensMount {
    LensId lens_id;
    GenerationId generation;
    ArtifactHash artifact;
    LensManifest manifest;
    std::shared_ptr<ILens> lens;
    std::shared_ptr<MountGuard> guard;
};

struct LightPathSnapshot {
    MountEpoch epoch;
    std::vector<std::shared_ptr<const LensMount>> ordered;
    Hash path_hash;
};
```

`LightPathSnapshot` 发布后不可变。一拍开始时取得 `shared_ptr<const LightPathSnapshot>`；这一拍不受并发换镜影响。

### 6.3 MountGuard

`MountGuard` 是纯透镜体系的镜座电源与资源托盘。Lens 想创建下列对象，必须通过 guard 提供的 `OpticalHost`：

- `std::jthread` 或 coroutine runner；
- timer；
- bounded channel endpoint；
- socket 与 HTTP stream；
- file watcher；
- child process 与 PTY；
- dynamic library handle；
- temporary artifact；
- callback registration。

Guard 维护枚举表和停止源，但不保存业务事实。拔镜顺序为：

```text
remove mount from next LightPath
→ reject new BeamTicket
→ request_stop for hosted activities
→ wait bounded afterglow deadline
→ terminate worker/process tree if required
→ close handles and release code image
→ append lens.afterglow-ended
```

这套机制只负责停止未来活动和回收进程资源。已经 committed 的 Photon 和已经发生的外部结果不受影响。

### 6.4 BeamTicket

每次 `refract` 获得一个 `BeamTicket`：

```cpp
struct BeamTicket {
    BeamId beam_id;
    MountEpoch epoch;
    GenerationId target_generation;
    std::stop_token stop;
    Deadline deadline;
    RayBudget budget;
};
```

Ticket 固定目标 generation，避免换镜时调用跳到另一份代码。一旦开始的折射可以在 afterglow 时限内完成；新 Act 只寻址新 LightPath。

### 6.5 OpticalHost

Lens 不能直接取得 daemon 内部对象。Nyxia 按 manifest 给出窄接口：

```cpp
class OpticalHost {
public:
    virtual PhotonReader& photons() noexcept = 0;
    virtual PhotonEmitter& emitter() noexcept = 0;
    virtual ActGate& acts() noexcept = 0;
    virtual HostedIo& io() noexcept = 0;
    virtual HostedTasks& tasks() noexcept = 0;
    virtual SecretBinder& secrets() noexcept = 0;
    virtual ArtifactStore& artifacts() noexcept = 0;
};
```

实际可见方法由 manifest 中的 `light_permissions` 缩减。未知 native lens 只能通过 worker RPC 使用更窄的 host bridge。

### 6.6 Nyxia 更新

Nyxia 不在活动 daemon 内换代，因为它持有 LightPath 发布点、动态代码加载器、append/Act gate 连接和所有镜座。更新 Nyxia 必须：

1. launcher 验证新宿主；
2. 当前 daemon 停止接受新 ray；
3. 等待或终止在途 Beam；
4. Chora flush 并记录 `system.handoff-ready`；
5. 启动新 daemon，从 Photon 重建 LightPath；
6. Snow/Termon 重连；
7. 成功后关闭旧进程。

这不是业务透镜换代路径，也不允许静默原地覆盖可执行文件。

---

## 7. 双向透镜编程模型

### 7.1 C++20 核心接口

解释文档中的 `view/refract` 是唯一 Lens 形状。工程版本保留这一形状，同时用结构化 Act 和只追加 emitter 替代裸字符串与可变容器：

```cpp
#include <tl/expected.hpp>

template<class T>
using Result = tl::expected<T, Error>;

class ILens {
public:
    virtual ~ILens() = default;

    [[nodiscard]] virtual const LensManifest& manifest() const noexcept = 0;

    // 看：只读折叠，不允许外部写操作。
    virtual Result<void> view(
        const PhotonWindow& photons,
        SurfaceBuilder& surface) const = 0;

    // 做：Act 已通过固定准入管线；只能用 emitter 追加结果。
    virtual Task<Result<RefractionResult>> refract(
        const PhotonWindow& photons,
        const Act& act,
        RefractionBeam& beam) = 0;
};
```

`RefractionBeam` 组合 `BeamTicket`、受限 `OpticalHost` 和 `PhotonEmitter`。接口不返回可变 `CausalRay`，从类型上阻止覆写历史。

### 7.2 LensManifest

```cpp
struct LensManifest {
    LensId id;
    SemVer version;
    AbiVersion abi;
    std::int32_t optical_order;
    std::vector<SurfaceChannel> view_channels;
    std::vector<PhotonPatternSpec> photon_patterns;
    std::vector<ActPatternSpec> act_patterns;
    std::vector<LightPermission> light_permissions;
    Determinism view_determinism;
    IsolationMode isolation;
    Hash schema_bundle_hash;
};
```

Manifest 是签名 artifact 的一部分。运行时不能自行扩大 pattern、permission 或 surface channel。

### 7.3 Photon Pattern Matching

开发者不手写字符串扫描。SDK 根据 schema 把命中的 Photon/Act 解码成 C++20 类型：

```cpp
struct CalculateArgs {
    std::string expression;
};

inline constexpr auto calculate_pattern =
    act_pattern<CalculateArgs>("tool.calculate", "tokmon.math.calculate.v1");

auto match = calculate_pattern.match(act);
if (!match) {
    co_return RefractionResult::pass();
}

const CalculateArgs& args = match->value();
```

生成器从 JSON Schema/CBOR schema 生成：字段校验、最大长度、枚举、数值范围、unknown-field 策略和 C++ codec。任何校验失败都在执行前产生 `act.denied` Photon。

### 7.4 SurfaceBuilder

`view` 不能直接改另一个 Lens 的对象。它只向声明过的 channel 追加不可变 contribution：

```cpp
class SurfaceBuilder {
public:
    ModelSurfaceWriter& model();
    UiSurfaceWriter& ui();
    CliSurfaceWriter& cli();
    ActSurfaceWriter& acts();
    DiagnosticSurfaceWriter& diagnostics();
};
```

Writer 自动附上 `origin_lens`、generation、source seq 和 precedence。冲突由 channel 的确定性折叠规则处理，不由调用顺序之外的隐藏状态处理。

### 7.5 C ABI

C++ façade 只用于同一构建工具链。动态二进制边界使用版本化 C ABI：

```c
typedef struct TokmonBytesV1 {
    const unsigned char* data;
    size_t size;
} TokmonBytesV1;

typedef struct TokmonOwnedBytesV1 {
    unsigned char* data;
    size_t size;
    void (*release)(unsigned char*, size_t, void*);
    void* user;
} TokmonOwnedBytesV1;

typedef struct TokmonLensApiV1 {
    uint32_t abi_version;
    TokmonBytesV1 (*manifest)(void* instance);
    int32_t (*view)(void* instance,
                    TokmonBytesV1 photon_window,
                    TokmonOwnedBytesV1* surface_delta);
    int32_t (*refract)(void* instance,
                       TokmonBytesV1 photon_window,
                       TokmonBytesV1 act,
                       const TokmonHostApiV1* host,
                       TokmonOwnedBytesV1* result);
    void (*request_stop)(void* instance);
    void (*destroy)(void* instance);
} TokmonLensApiV1;

TOKMON_LENS_EXPORT TokmonLensApiV1 tokmon_lens_entry_v1(void);
```

边界传 canonical CBOR frame；`tl::expected` 在 adapter 中编码成返回码和结构化错误 frame。C++ 异常、STL 容器、allocator 所有权、coroutine frame 和 RTTI 不跨 ABI。

### 7.6 支持的 Lens 运行形态

| 形态 | 适用 | 默认信任 | 换代方式 |
| --- | --- | --- | --- |
| in-process C++ | 官方且签名的热路径 Lens | 高 | R1 generation swap |
| worker C ABI | 第三方 native Lens | 中/低 | 进程 handoff |
| Node.js worker | JavaScript Lens、编译后的 TypeScript Lens、npm 生态 | 中/低 | 进程 handoff |
| CPython worker | Python Lens、PyPI 生态、数据/RAG/自动化 | 中/低 | 进程 handoff |
| WASM | 纯计算、转换、策略 Lens | 低 | instance swap |
| desktop process | Termon/Slint | UI trust | launcher handoff |

来源未知的 native code 不能因“实现了 ILens”就进入 daemon 地址空间。Node.js 与 CPython 永远不嵌入或链接进 `tokmond`；它们只存在于受 Styx 限制、按需启动的独立 worker 进程。

TypeScript 是开发语言，不是生产解释路径：构建阶段必须编译为 Node.js 可执行的 ESM JavaScript，artifact 只执行 `.mjs/.js`。JavaScript 可直接提供 ESM。Python artifact 使用锁定的 CPython 版本和 module entry。

生产模式不解析系统 `PATH` 上碰巧安装的 `node`/`python`，而从 `.tokmon/runtimes/` 选择 hash 已验证的 exact runtime。开发模式可以显式选择本机 runtime 做快速迭代，但该结果标为 non-reproducible，不能直接进入已签名 active LightPath。

### 7.7 Lens Worker Protocol v1

`tokmond` 中的 `WorkerLensProxy` 实现 C++ `ILens`，把 `view/refract` 映射到 worker RPC。Node.js、CPython 和低信任 native worker 共享同一协议：

```text
tokmond / WorkerLensProxy
        │ canonical CBOR frames
        ▼
tokmon-lens-worker --runtime {native|node|cpython}
        │ language SDK
        ▼
user Lens.view / Lens.refract
```

`tokmon-lens-worker` 是 C++20 sandbox supervisor。对 Node.js/CPython，它先建立 OS 限制、专用 IPC 与私有临时目录，再在同一受控 process tree 中启动 exact runtime 和语言 adapter；supervisor 持有 deadline、heartbeat、stdout/stderr 配额与整棵进程树的最终终止权。

传输使用专用 named pipe/Unix domain socket 或 launcher 创建的匿名 pipe。协议与 stdout/stderr 分离，避免用户输出破坏 frame。

核心 frame：

| 方向 | Frame | 含义 |
| --- | --- | --- |
| host → worker | `worker.hello` | protocol、generation、runtime、limits、nonce |
| worker → host | `worker.ready` | manifest/schema hash、SDK/runtime version |
| host → worker | `lens.view.request` | request id、epoch、PhotonWindow、允许 channel |
| worker → host | `lens.view.result` | `SurfaceDelta` 或 `ErrorFrame` |
| host → worker | `lens.refract.request` | BeamTicket、Act、PhotonWindow、deadline |
| worker → host | `lens.refract.result` | `RefractionResult` 或 `ErrorFrame` |
| worker → host | `host.call` | emit PhotonDraft、blob、artifact、secret、I/O 请求 |
| host → worker | `host.result` | 受控 host 调用结果 |
| host → worker | `beam.cancel` | stop 指令 |
| host → worker | `worker.shutdown` | afterglow 结束，准备退出 |
| worker → host | `worker.stopped` | 已停止接收新调用并释放语言运行时资源 |

协议不传整个历史数据库，只传预算后的 PhotonWindow、cursor 和 blob reference。每个 frame 有：长度上限、嵌套深度、request id、generation、epoch、deadline 和 canonical encoding 校验。

#### 错误映射

C++ 端继续使用 `tl::expected<T, Error>`。其他语言在 SDK 内使用自己的显式 Result：

```typescript
export type Result<T> =
  | { ok: true; value: T }
  | { ok: false; error: LensError };
```

```python
@dataclass(frozen=True)
class Result(Generic[T]):
    value: T | None = None
    error: LensError | None = None
```

跨 worker 边界统一编码为 `ErrorFrame`；JavaScript rejected Promise、CPython 未捕获异常和进程崩溃都由 worker 最外层转换为 `lens.crashed`，不能穿过协议边界。

#### Host API

Node.js/CPython Lens 不能直接访问 daemon 对象，只能请求：

```text
photon.emit
blob.read
artifact.write
act.request
secret.bind
io.http
io.process
io.workspace
log.write
```

`lens.yaml` 没有声明的调用在 host 侧拒绝。需要触碰现实的请求仍进入 Techor → Fallen → Cista → Styx 固定管线，语言 worker 不能自行绕开。

### 7.8 错误与取消

- C++ 宿主与 native Lens 的可预期失败统一返回 `tl::expected<T, Error>`；项目别名为 `Result<T>`；
- `view` 错误产生诊断 contribution；关键 Surface 无法建立时该拍失败；
- `refract` 返回 `Result<RefractionResult>`，错误必须映射为结构化 Photon；
- stop 通过 `BeamTicket.stop` 传递；
- deadline、输出大小、CPU 和子进程限制由 RefractionBeam 强制；
- C++ 核心路径和 C++ Lens 业务代码不用异常表达校验、I/O、解析或拒绝；
- 第三方库若抛出异常，只能在最外层 adapter 捕获并转换为 `Error`；越过 C ABI 的异常视为 `lens.crashed`，worker 随后隔离退出。
- Node.js worker 使用 `AbortSignal` 映射 `beam.cancel`；CPython worker 使用 SDK cancellation event/`asyncio` task cancellation；
- 两种运行时的内存、CPU、进程树、文件、网络、输出和 deadline 由 Styx/OS 边界强制，语言运行时参数只能作为附加限制；
- cooperative cancel 到期后由宿主终止整个 worker 进程树；
- worker 心跳只用于故障检测，不是 Photon；worker exit/crash 的观察结果必须追加结构化 Photon。

---

## 8. 开发者实战：5 分钟编写动态透镜

本节是 `tokmon-lens-architecture-explained.zh.md` 第 7 章的 C++20 正式实现版。开发者只需要声明光斑、贡献 `view`、实现真实 `refract`，无需注册监听器或连接其他 Lens。

### 8.1 文件结构

```text
calculator-lens/
├─ CMakeLists.txt
├─ lens.yaml
├─ schemas/
│  ├─ calculate.args.schema.json
│  └─ calculate.result.schema.json
└─ src/
   └─ calculator_lens.cpp
```

### 8.2 `lens.yaml`

```yaml
id: org.tokmon.lens.calculator
version: 1.0.0
abi: 1
optical_order: 720
view_channels:
  - model.tools
act_patterns:
  - tool.calculate@tokmon.math.calculate.v1
light_permissions:
  - photon.emit:tool.result
  - photon.emit:tool.error
isolation: worker
```

### 8.3 完整 Lens

```cpp
#include <tokmon/lens/sdk.hpp>
#include <tl/expected.hpp>

#include <charconv>
#include <string>
#include <string_view>
#include <utility>

namespace calculator {

struct CalculateArgs {
    std::string expression;
};

enum class ParseErrorCode {
    NumberExpected,
    MissingRightParenthesis,
    DivisionByZero,
    UnexpectedToken
};

struct ParseError {
    ParseErrorCode code;
    std::size_t offset;
    std::string message;
};

using ParseResult = tl::expected<double, ParseError>;

class Parser final {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    ParseResult parse() {
        auto value = expression();
        if (!value) return tl::make_unexpected(std::move(value.error()));
        spaces();
        if (pos_ != text_.size()) {
            return fail(ParseErrorCode::UnexpectedToken, "unexpected token");
        }
        return *value;
    }

private:
    ParseResult expression() {
        auto lhs = term();
        if (!lhs) return tl::make_unexpected(std::move(lhs.error()));
        double value = *lhs;

        for (;;) {
            spaces();
            if (take('+')) {
                auto rhs = term();
                if (!rhs) return tl::make_unexpected(std::move(rhs.error()));
                value += *rhs;
            } else if (take('-')) {
                auto rhs = term();
                if (!rhs) return tl::make_unexpected(std::move(rhs.error()));
                value -= *rhs;
            } else {
                return value;
            }
        }
    }

    ParseResult term() {
        auto lhs = factor();
        if (!lhs) return tl::make_unexpected(std::move(lhs.error()));
        double value = *lhs;

        for (;;) {
            spaces();
            if (take('*')) {
                auto rhs = factor();
                if (!rhs) return tl::make_unexpected(std::move(rhs.error()));
                value *= *rhs;
            } else if (take('/')) {
                auto rhs = factor();
                if (!rhs) return tl::make_unexpected(std::move(rhs.error()));
                if (*rhs == 0.0) {
                    return fail(ParseErrorCode::DivisionByZero, "division by zero");
                }
                value /= *rhs;
            } else {
                return value;
            }
        }
    }

    ParseResult factor() {
        spaces();
        if (take('(')) {
            auto value = expression();
            if (!value) return tl::make_unexpected(std::move(value.error()));
            spaces();
            if (!take(')')) {
                return fail(ParseErrorCode::MissingRightParenthesis, "missing ')'");
            }
            return *value;
        }

        const char* first = text_.data() + pos_;
        const char* last = text_.data() + text_.size();
        double value{};
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{}) {
            return fail(ParseErrorCode::NumberExpected, "number expected");
        }
        pos_ = static_cast<std::size_t>(parsed.ptr - text_.data());
        return value;
    }

    ParseResult fail(ParseErrorCode code, std::string message) const {
        return tl::make_unexpected(ParseError{code, pos_, std::move(message)});
    }

    void spaces() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t')) ++pos_;
    }

    bool take(char ch) {
        if (pos_ < text_.size() && text_[pos_] == ch) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::string_view text_;
    std::size_t pos_{};
};

class CalculatorLens final : public tokmon::ILens {
public:
    const tokmon::LensManifest& manifest() const noexcept override {
        return manifest_;
    }

    tokmon::Result<void> view(
        const tokmon::PhotonWindow&,
        tokmon::SurfaceBuilder& surface) const override {
        surface.model().add_tool(tokmon::ToolSchema{
            .name = "calculate",
            .description = "计算包含括号及 + - * / 的数学表达式",
            .arguments_schema = "tokmon.math.calculate.v1",
            .origin_lens = manifest_.id
        });
        return {};
    }

    tokmon::Task<tokmon::Result<tokmon::RefractionResult>> refract(
        const tokmon::PhotonWindow&,
        const tokmon::Act& act,
        tokmon::RefractionBeam& beam) override {
        auto matched = calculate_.match(act);
        if (!matched) {
            co_return tokmon::RefractionResult::pass();
        }

        auto parsed = Parser{matched->expression}.parse();
        if (!parsed) {
            co_await beam.emitter().emit(tokmon::PhotonDraft::tool_error(
                act, "calculate", parsed.error().message));
            co_return tokmon::RefractionResult::failed("invalid_expression");
        }

        co_await beam.emitter().emit(tokmon::PhotonDraft::tool_result(
            act, "calculate", tokmon::encode_number(*parsed)));
        co_return tokmon::RefractionResult::completed();
    }

private:
    inline static const tokmon::ActPattern<CalculateArgs> calculate_{
        "tool.calculate", "tokmon.math.calculate.v1"};
    inline static const tokmon::LensManifest manifest_ =
        tokmon::load_embedded_manifest();
};

} // namespace calculator

TOKMON_EXPORT_LENS(calculator::CalculatorLens)
```

示例是真实表达式求值，不使用 `eval`，也不返回写死结果。生产 codec 由 schema generator 生成；上面省略生成文件的 include。

### 8.4 CMake

```cmake
cmake_minimum_required(VERSION 3.25)
project(calculator_lens LANGUAGES CXX)

find_package(tokmon-lens-sdk CONFIG REQUIRED)
find_package(tl-expected CONFIG REQUIRED)

add_library(calculator_lens SHARED src/calculator_lens.cpp)
target_compile_features(calculator_lens PRIVATE cxx_std_20)
target_link_libraries(calculator_lens PRIVATE Tokmon::LensSDK tl::expected)

tokmon_embed_lens_manifest(calculator_lens lens.yaml)
tokmon_generate_lens_codecs(calculator_lens schemas)
tokmon_sign_lens_artifact(calculator_lens)
```

### 8.5 安装与验证

```text
tokmon lens inspect ./calculator-lens
tokmon lens verify ./calculator-lens
tokmon lens mount ./calculator-lens --dark-lane
tokmon lens test ./calculator-lens --golden tests/calculator.ray
tokmon lens activate org.tokmon.lens.calculator@1.0.0
```

开发闭环：

```text
定义 schema
→ view 暴露 tool surface
→ pattern 自动萃取强类型参数
→ refract 真实计算
→ emitter 追加结果 Photon
→ 下一拍 view 自动看到结果
```

### 8.6 零横向调用

CalculatorLens 不知道 Rhea、Techor、Chora 或 Termon 的 C++ 类型：

- Rhea 只消费 `ModelSurface`；
- Techor 从模型 Photon 生成 `tool.calculate` Act；
- Nyxia 根据 ActPattern 寻址 CalculatorLens；
- CalculatorLens 通过 emitter 追加结果；
- Chora 执行真实提交；
- Termon 下一次 `view` 折叠显示结果。

这条路径是线性的，复杂度随镜片数量增长，不形成两两连接。

### 8.7 TypeScript / JavaScript Lens

TypeScript 和 JavaScript 共用 `@tokmon/lens-sdk` 与 Node.js worker。TypeScript 在构建阶段编译为 ESM；JavaScript 直接编写 ESM。两者可以使用 npm 依赖，只要依赖被锁定、打包、审计并符合 worker 的 OS 权限。以下示例执行真实加法并追加结果 Photon：

```typescript
import {
  actPattern,
  defineLens,
  ok,
  type Act,
  type PhotonWindow,
  type RefractionBeam,
  type Result,
  type SurfaceBuilder,
} from "@tokmon/lens-sdk";

type AddArgs = {
  left: number;
  right: number;
};

const add = actPattern<AddArgs>(
  "tool.add",
  "tokmon.math.add.v1",
);

export default defineLens({
  id: "org.tokmon.lens.adder-ts",

  view(
    _photons: PhotonWindow,
    surface: SurfaceBuilder,
  ): Result<void> {
    surface.model.addTool({
      name: "add",
      description: "计算两个数字之和",
      argumentsSchema: "tokmon.math.add.v1",
    });
    return ok(undefined);
  },

  async refract(
    _photons: PhotonWindow,
    act: Act,
    beam: RefractionBeam,
  ) {
    const matched = add.match(act);
    if (!matched.ok) return ok({ status: "pass" });

    const result = matched.value.left + matched.value.right;
    const emitted = await beam.emitter.toolResult(act, "add", { result });
    if (!emitted.ok) return emitted;
    return ok({ status: "completed" });
  },
});
```

JavaScript 版本使用相同 API，只移除 TypeScript 类型标注。生产 artifact 不执行 `.ts`，也不在用户机器上启动 TypeScript 编译器。

```yaml
id: org.tokmon.lens.adder-ts
version: 1.0.0
abi: 1
runtime:
  kind: node
  version: "<exact-version-from-lens-lock>"
  entry: dist/index.mjs
  module: esm
view_channels:
  - model.tools
act_patterns:
  - tool.add@tokmon.math.add.v1
light_permissions:
  - photon.emit:tool.result
isolation: worker
```

```text
adder-ts/
├─ lens.yaml
├─ schema.bundle.cbor
├─ dist/index.mjs
├─ package.json
├─ package-lock.json
├─ dependency-bundle/
├─ sbom.spdx.json
├─ checksums.txt
└─ signature.sig
```

构建使用 lockfile 和关闭任意 install script 的受控环境。运行时禁止 `npm install`、网络拉包和动态改变 dependency graph；Node.js runtime、SDK、入口文件和依赖 hash 全部写入 `lens-lock.yaml`。

- 纯 JavaScript 依赖优先 bundle 到 `dist/`；
- 必须保留 module resolution 的依赖打包成只读 `dependency-bundle/`；
- 使用 native addon/N-API 的 artifact 必须按 OS、architecture、Node ABI 分包；
- install script 默认禁止，确需运行时只能在构建隔离环境执行，产物随后重新 hash、生成 SBOM 并签名；
- Node.js worker 不继承用户 shell 的 `NODE_PATH`、startup preload 或任意环境变量。

### 8.8 CPython Lens

Python 使用 `tokmon-lens-sdk` 与独立 CPython worker，可以复用 PyPI、纯 Python wheel 和平台匹配的 CPython 扩展 wheel。示例保持相同 `view/refract` 语义：

```python
from dataclasses import dataclass

from tokmon_lens_sdk import (
    Act,
    ActPattern,
    Lens,
    PhotonWindow,
    RefractionBeam,
    Result,
    SurfaceBuilder,
    completed,
    ok,
    passed,
)


@dataclass(frozen=True)
class AddArgs:
    left: float
    right: float


class AdderLens(Lens):
    id = "org.tokmon.lens.adder-python"
    add = ActPattern[AddArgs]("tool.add", "tokmon.math.add.v1")

    def view(
        self,
        _photons: PhotonWindow,
        surface: SurfaceBuilder,
    ) -> Result[None]:
        surface.model.add_tool(
            name="add",
            description="计算两个数字之和",
            arguments_schema="tokmon.math.add.v1",
        )
        return ok(None)

    async def refract(
        self,
        _photons: PhotonWindow,
        act: Act,
        beam: RefractionBeam,
    ) -> Result:
        matched = self.add.match(act)
        if not matched.ok:
            return ok(passed())

        result = matched.value.left + matched.value.right
        emitted = await beam.emitter.tool_result(
            act=act,
            tool_name="add",
            payload={"result": result},
        )
        if not emitted.ok:
            return emitted
        return ok(completed())
```

```yaml
id: org.tokmon.lens.adder-python
version: 1.0.0
abi: 1
runtime:
  kind: cpython
  version: "<exact-version-from-lens-lock>"
  entry: tokmon_adder:AdderLens
view_channels:
  - model.tools
act_patterns:
  - tool.add@tokmon.math.add.v1
light_permissions:
  - photon.emit:tool.result
isolation: worker
```

```text
adder-python/
├─ lens.yaml
├─ schema.bundle.cbor
├─ src/tokmon_adder/
├─ requirements.lock
├─ wheels/
├─ sbom.spdx.json
├─ checksums.txt
└─ signature.sig
```

CPython runtime 规则：

- CPython major/minor/patch、platform、architecture 和 extension ABI 必须精确锁定；
- 每个 artifact hash 使用独立、不可变的 virtual environment 或预组装 runtime image；
- 依赖由带 sha256 的 lockfile 固定，并从签名 wheel bundle 离线安装；装镜期间禁止访问公共 package index；
- 禁止共享可写 `site-packages`；
- CPython C extension 必须位于平台专用 artifact，经过符号、许可证和漏洞扫描；
- GIL 只影响当前 worker，需要并行时启动多个 worker，不在 daemon 中创建子解释器；
- module global 不是规范状态，worker 重启后必须能从 PhotonWindow 重建结果。

### 8.9 npm/PyPI 包导入边界

采用完整 Node.js/CPython 的目的就是保留模块与包生态。Lens 可以正常使用静态 `import`、受控 dynamic import、Python package import、纯语言包和平台匹配的 native dependency；限制的是依赖何时进入系统以及它能触碰哪些现实边界，而不是禁止导包。

```text
developer declares dependencies
→ isolated build resolves exact graph
→ lock every transitive version + source + hash
→ bundle JS packages / collect Python wheels
→ scan licenses, vulnerabilities and native code
→ generate SBOM + dependency-tree hash
→ sign Lens artifact
→ dark lane materializes immutable environment offline
→ worker imports only from that environment
```

Node.js module search root 仅包含 runtime built-in modules、`@tokmon/lens-sdk`、Lens payload 与只读 `dependency-bundle/`。不读取全局 npm 包、用户 `node_modules`、`NODE_PATH` 或联网模块；dynamic import 的最终文件也必须属于已签名内容。

CPython `sys.path` 仅包含锁定 stdlib、`tokmon-lens-sdk`、Lens `src/` 与该 artifact 的只读 `site-packages`。禁用 user-site、editable install、任意 `.pth` 注入和运行时 package-index 访问。

诸如 lodash、zod、parser、NumPy、pandas 等本地计算依赖可以直接使用。任何包若尝试网络、workspace 写入、进程启动、Secret 获取或 Photon 提交，仍必须改接 SDK 的 `io.*`/`act.request`/`photon.emit`；worker 的直接网络与任意宿主路径访问在 OS 层被拒绝。包生态不是第二条 Act 管线。

### 8.10 跨语言一致性

- C++、JavaScript/TypeScript、Python 使用同一 manifest/schema/Photon/Act/Surface 语义；
- 非 C++ SDK 不得出现另一套事件总线、状态存储或直接 Lens lookup；
- `view` 默认不得进行网络、文件写入或长时间阻塞；
- `refract` 的所有 host 调用受 BeamTicket、deadline、budget 和 permission 控制；
- SDK 的 `host.log` 发送结构化日志 frame，由 C++ host 脱敏后写入 `spdlog`；
- stdout/stderr 作为有界诊断流捕获，不作为 worker 协议，也不形成 committed Photon，除非某个 Act 明确要求记录；
- JavaScript/Python 可贡献通用 UiSurface 数据和内置卡片 schema，但不能在运行时注入任意 Slint 代码；
- 相同 golden ray 必须可以分别驱动 C++、Node.js 和 CPython 的等价测试 Lens。

---

## 9. 动态装卸与运行时替换

### 9.1 替换等级

| 等级 | 含义 | 适用 |
| --- | --- | --- |
| R0 | 只换配置或不可执行数据 | schema 文案、theme、静态 instruction |
| R1 | 同进程新 generation 接管新光束 | 官方签名 C++ Lens |
| R2 | worker/进程 handoff | 第三方 native、Node.js、CPython Lens 与 Termon |
| R3 | 宿主完整交接 | Nyxia 更新 |

除 Nyxia 外的十九个命名透镜至少达到 R1；Termon 固定使用 R2；低信任 native code 固定使用 R2。

### 9.2 Lens artifact

```text
artifact/
├─ lens.yaml
├─ schema.bundle.cbor
├─ payload/
│  ├─ native/<platform>/<arch>/lens binary       # native 形态
│  ├─ node/dist + dependency-bundle              # Node.js 形态
│  ├─ cpython/src + wheel bundle + lockfile       # CPython 形态
│  └─ wasm/module.wasm                            # WASM 形态
├─ assets/
├─ sbom.spdx.json
├─ checksums.txt
└─ signature.sig
```

每个 artifact 只包含 manifest 指定的一个 runtime payload。artifact 以内容哈希寻址；签名覆盖 manifest、schema、runtime payload、dependency lock、assets、SBOM 和 checksum。

Node.js/CPython artifact 必须同时锁定：

- runtime kind 和精确 runtime version；
- OS、architecture；
- Node ABI 或 CPython extension ABI（存在 native dependency 时）；
- SDK protocol version；
- 每个依赖的版本、来源和 sha256；
- 生成后的 dependency tree hash；
- SBOM 与许可证结果。

### 9.3 用 `.tokmon/light-path.yaml` 表达期望光路

用户级 `~/.tokmon/light-path.yaml` 定义全局默认光路，项目级 `<workspace>/.tokmon/light-path.yaml` 在其上做项目覆盖。文件描述的是 **desired LightPath**，不是当前正在运行的事实。

```yaml
api_version: tokmon.dev/v1
mode: overlay

lenses:
  - lens_id: org.tokmon.lens.calculator
    state: mounted
    version: 1.0.0
    artifact_sha256: 8a6d7c...f21b
    optical_order: 720
    isolation: worker

  - lens_id: org.tokmon.lens.legacy-calculator
    state: absent
```

规则：

- `state: mounted` 表示希望装入或保持该精确 generation；
- `state: absent` 表示希望从后续 LightPath 拔出；
- active generation 必须解析到 `lens-lock.yaml` 中的精确 artifact hash，运行时不跟随浮动版本；
- 项目级条目按 `lens_id` 覆盖用户级条目；
- `optical_order` 冲突按 `(optical_order, lens_id)` 稳定排序，但 ActPattern 冲突必须拒绝；
- 修改 YAML 只产生候选，不能直接修改 active LightPath。

对应命令：

```text
tokmon lens install ./calculator-lens --user
tokmon lens mount org.tokmon.lens.calculator@1.0.0 --project
tokmon lens replace org.tokmon.lens.calculator ./calculator-lens-v1.1 --project
tokmon lens unmount org.tokmon.lens.calculator --project
tokmon lens status --light-path
```

命令行按照选择的层级原子写入 `light-path.yaml`/`lens-lock.yaml`，然后等待 daemon 返回 committed mount epoch。写文件使用 temporary sibling + fsync + atomic rename，禁止让 watcher 看到半份 YAML。

### 9.4 从 YAML 变化到 Photon

运行中的组合变化必须进入 Fact → Lens → Act 闭环：

```text
user/project .tokmon/light-path.yaml changes
→ Cove/control watcher reads complete file
→ yaml-cpp parse + schema validation
→ append config.light-path-observed(hash, source, normalized desired path)
→ Ignis.view compares desired path with active mount epoch
→ Ignis.refract proposes lens.reconcile Act
→ Fallen checks trust/permission/isolation differences
→ candidate artifacts enter dark lane
→ append lens.candidate-validated or lens.rejected
→ append mount.epoch-committed
→ Nyxia atomically publishes new LightPathSnapshot
```

因此，YAML 是人类可编辑的期望输入；最后 committed mount epoch Photon 才是恢复 active LightPath 的依据。文件被再次编辑、删除或回滚，也只会产生新的观察和新 epoch，不改写旧 Photon。

Watcher 规则：

- 用户级和项目级目录分别监听，统一做 100–300 ms debounce；
- 以内容 hash 去重，不依赖不稳定的 OS watcher 次数或顺序；
- rename、truncate、重复通知和编辑器临时文件不能产生半成品配置；
- YAML parse 失败追加 `config.rejected`，继续使用当前 LightPath；
- daemon 启动时读取文件并与最后 committed epoch 对比，不能盲目重复换镜。

### 9.5 Dark lane

候选 generation 不直接进入主光路：

```text
verify artifact and signature
→ create isolated LensMount + MountGuard
→ call manifest and ABI smoke
→ start selected runtime worker and protocol handshake
→ verify Node.js/CPython runtime and dependency tree hash
→ replay selected PhotonWindow through view
→ compare SurfaceDelta against policy/golden
→ run synthetic Act patterns in sandbox
→ test stop/deadline/output bounds
→ append lens.candidate-validated
→ propose mount.commit Act
```

Dark lane 的输出不能进入用户主光流，只有验证报告摘要能作为 Photon 提交。

### 9.6 原子换镜

```text
old path epoch E
→ build complete candidate path E+1
→ append lens.replaced + mount.epoch-committed photons
→ atomic_store(shared_ptr<const LightPathSnapshot E+1>)
→ new steps acquire E+1
→ E in-flight BeamTicket enters afterglow
→ old guard closes after deadline
```

提交顺序必须是新 epoch Photon durable 在前，LightPath 发布在后。若进程在两者之间崩溃，重启按最后 committed epoch 完成或回到先前完整 epoch，并追加 recovery Photon。

C++20 发布点只交换一枚不可变 shared pointer：

```cpp
class ActiveLightPath final {
public:
    std::shared_ptr<const LightPathSnapshot> load() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

    std::shared_ptr<const LightPathSnapshot> publish(
        std::shared_ptr<const LightPathSnapshot> candidate) noexcept {
        return active_.exchange(std::move(candidate), std::memory_order_acq_rel);
    }

private:
    std::atomic<std::shared_ptr<const LightPathSnapshot>> active_;
};
```

每个 engine step 在开始时 `load()` 一次并持有 shared pointer；每个 BeamTicket 再固定目标 generation。因此交换期间：

- 新 step 只看到完整 E+1；
- 已开始 step 继续看到完整 E；
- 不存在一半旧、一半新的 LightPath；
- 旧动态库只有在没有调用栈、Beam 和 path 引用后才允许卸载。

### 9.7 Afterglow 与安全卸载

原子交换只解决“谁接新光束”，安全卸载还需要：

1. 从新 LightPath 删除旧 mount；
2. MountGuard 拒绝新 BeamTicket；
3. 等待旧 generation 已持有的 Beam；
4. deadline 到达后请求 stop；
5. worker 形态终止旧进程并关闭 IPC；
6. in-process 形态等待所有 shared pointer 与调用栈退出，再 `FreeLibrary/dlclose`；
7. 关闭 watcher/socket/timer/thread/process 等托管对象；
8. 追加 `lens.afterglow-ended`。

第三方 native Lens 默认使用 worker，是最可靠的热插拔路径。Node.js 与 CPython Lens 则强制使用 worker。换代等价于启动携带新 runtime/dependency environment 的 worker、完成 dark-lane handshake、切换路由、终止旧 worker；不同 runtime version 可以在换代窗口内短暂并存。

### 9.8 零新贡献定义

镜片从 LightPath 移除后：

- 新 `view` 折叠不再调用它；
- 新 Act pattern 不再匹配它；
- 新 BeamTicket 不再签发给它；
- 它贡献过的最终 Prompt/UI 并未保存，下一次显像自然消失；
- 历史 Photon 继续存在，但只作为历史；
- 在途折射只可在 afterglow 时限内完成并标记旧 generation；
- 超时活动由 guard 停止，结果追加为 cancelled/unknown Photon。

零新贡献不等于删除历史，也不等于撤回已经发生的现实动作。

### 9.9 不同 Lens 的特殊交接

| Lens 类别 | 额外交接规则 |
| --- | --- |
| 普通纯计算 Lens | atomic path swap 后等待旧 Beam 结束 |
| Rhea | 已发出的模型调用留在旧 generation；新调用走新 generation |
| Lemon | 建立 E↔E+1 bridge，旧 cursor/queue 排空后关闭 |
| Snow | 新 listener 就绪并接管新连接，旧连接在 deadline 内完成 |
| Chora | 短 commit barrier，唯一 writer token 从旧 generation 交给新 generation |
| Termon | launcher 启动新 desktop，同步 cursor 后切换可见窗口 |
| worker Lens | 新进程 handshake 后原子切 IPC endpoint，旧进程 afterglow |
| Node.js Lens | 新 runtime/dependency bundle ready 后切 worker endpoint |
| CPython Lens | 新 immutable environment ready 后切 worker endpoint |

任何交接失败都不发布半成品 LightPath。若故障在 E+1 已发布后才暴露，恢复旧 artifact 必须创建 E+2，而不是修改或撤销 E+1。

### 9.10 Ignis 自身换代

Ignis 的候选由当前 Ignis 准备，但最终 LightPath CAS 由 Nyxia 执行。候选不能批准自身；Fallen 验证签名、来源和 permission 差异。失败时当前 Ignis 仍保留，新 epoch 不发布。

---

## 10. RayTracingEngine：光流发动机

### 10.1 与解释文档 6.5 的对应

正式发动机保持原模型的六个核心步骤：

1. 从 Chora 读取当前 PhotonWindow；
2. 沿 LightPath 对所有 Lens 执行单向 `view` 折叠；
3. Rhea 根据 `ModelSurface` 进行真实模型调用并追加响应 Photon；
4. Techor 从结构化模型响应中萃取 Act；
5. Act 沿固定安全光路进入目标 Lens 的 `refract`；
6. 真实结果作为新 Photon 追加，驱动下一拍；没有新 Act 时自然停机。

正式实现不在发动机里写死模型响应、不解析 `CALL_TOOL:` 文本标记，而是使用 provider 原生 structured tool call 和 schema codec。

### 10.2 Engine 状态

```cpp
enum class RayPhase : std::uint8_t {
    NeedView,
    NeedModel,
    ExtractActs,
    RefractActs,
    AwaitReality,
    Darkened,
    Cancelled,
    Failed
};

struct RayState {
    RayId id;
    StreamId stream;
    std::uint64_t observed_seq{};
    std::uint32_t step{};
    RayPhase phase{RayPhase::NeedView};
    MountEpoch epoch{};
    RayBudget remaining;
    std::vector<Act> pending_acts;
    std::vector<BeamId> awaiting_beams;
};
```

`RayState` 的规范变化也通过 `ray.*` Photon 表达；内存对象只是当前推进缓存。

### 10.3 一拍算法

```text
step(ray):
  1. path    = Nyxia.current_light_path()
  2. window  = Chora.read(ray.stream, ray.observed_seq + 1 .. tail)
  3. surface = empty SurfaceBuilder(path.epoch, tail)
  4. for lens in path.ordered:
       lens.view(window, surface)              // pull-based linear fold
  5. seal surface; append ray.surface-built

  6. if surface requests a model turn:
       propose model.call Act
       run Rhea.refract through Act pipeline
       append model response Photons

  7. acts = Techor.extract(new model/tool/input Photons)
  8. validate and append act.proposed for every Act

  9. if acts.empty and no awaiting_beams:
       append ray.darkened
       stop

 10. for act in deterministic order:
       admission = Fallen/Cista/Styx optical fold
       if denied: append act.denied
       else:
         target = path.match(act.kind, act.schema)
         ticket = Nyxia.issue_beam(target, ray budget)
         target.refract(window, act, ticket)
         PhotonEmitter appends result

 11. advance observed_seq and step
 12. if new committed Photon can create Act: schedule next step
      else if no awaiting beam: darken
```

### 10.4 C++20 主循环骨架

```cpp
Task<Result<RayOutcome>> RayTracingEngine::run(RayId id) {
    RayState ray = co_await restore_ray(id);

    while (!ray.remaining.exhausted()) {
        if (ray_stop_.stop_requested()) {
            co_await photons_.emit(PhotonDraft::ray_cancelled(ray));
            co_return RayOutcome::cancelled();
        }

        const auto path = nyxia_.light_path();
        const auto window = co_await photons_.read_after(
            ray.stream, ray.observed_seq, limits_.max_window_photons);

        SurfaceBuilder builder{path->epoch, window.tail_seq()};
        for (const auto& mount : path->ordered) {
            auto viewed = mount->lens->view(window, builder);
            if (!viewed) {
                co_await emit_view_error(ray, *mount, viewed.error());
                if (mount->manifest.view_determinism == Determinism::Critical) {
                    co_return RayOutcome::failed("critical_view_failed");
                }
            }
        }

        const SealedSurfaces surfaces = builder.seal();
        co_await maybe_call_model(ray, surfaces.model(), *path);

        auto acts = techor_.extract(
            co_await photons_.read_after(ray.stream, window.tail_seq(),
                                         limits_.max_step_photons));

        if (acts.empty() && ray.awaiting_beams.empty()) {
            co_await photons_.emit(PhotonDraft::ray_darkened(ray));
            co_return RayOutcome::darkened();
        }

        for (Act& act : acts) {
            co_await refract_one(ray, act, *path);
        }

        ray.observed_seq = co_await photons_.tail_seq(ray.stream);
        ++ray.step;
        ray.remaining.consume_step();
    }

    co_await photons_.emit(PhotonDraft::ray_failed(ray, "budget_exhausted"));
    co_return RayOutcome::failed("budget_exhausted");
}
```

`maybe_call_model` 调用真实 Rhea Lens；`refract_one` 调用真实准入与目标 Lens。测试可以注入确定性 Lens artifact，但生产代码不含模拟模型响应或写死工具结果。

### 10.5 动态萃取

Techor 只处理结构化响应：

```text
model.tool-call Photon
→ lookup (tool name, arguments schema) in current LightPath patterns
→ generated codec validates and decodes
→ construct typed Act envelope
→ append act.proposed
→ fixed admission pipeline
→ matched Lens.refract
```

找不到目标、schema 不匹配、参数越界或调用来自旧 epoch 时，产生相应 `act.denied` Photon，不猜测、不降级成 shell 字符串。

### 10.6 自然停机

一拍结束时同时满足以下条件，才追加 `ray.darkened`：

- Techor 没有萃取出新 Act；
- 没有 waiting approval；
- 没有在途 model/tool Beam；
- 没有已提交但未处理的新输入 Photon；
- 没有显式 workflow 下一步。

自然停机不是超时。预算、deadline、重复 Act 检测和最大步数仍是第二道保护：

```text
same (kind, args_hash, causal_parent) repeated N times
→ append ray.oscillation-detected
→ request approval or darken according to policy
```

### 10.7 并发与背压

- 同一 ray 的 committed step 串行；
- 不同 ray 可并行；
- 同一 Act 的多个独立只读 Beam 可按 manifest 并行；
- 写工作区的 Act 默认按 workspace 串行；
- model concurrency、worker count、PTY bytes 和 Photon batch 都有上限；
- Lemon 只传 cursor 和有界 frame，不复制整个 CausalRay；
- 慢消费者从 Chora 按 cursor 追赶，不迫使生产者持有无限内存。

### 10.8 回放

| 等级 | 含义 | 是否触碰现实 |
| --- | --- | --- |
| R0 Transcript | 从 Photon 重建对话、工具、Diff、状态 | 否 |
| R1 Surface | 用指定 LightPath 重建 Prompt/UI/CLI | 否 |
| R2 Control | 使用记录的模型和工具结果重演 engine step | 否 |
| R3 Live | 在新 fork 上重新调用模型和工具 | 是 |

R3 必须创建新 stream；它不能向旧 stream 插入或替换 Photon。

---

## 11. 二十透镜总览

| # | Lens | 光学称号 | `view` 主要贡献 | `refract` 主要动作 | 替换 |
| ---: | --- | --- | --- | --- | --- |
| 1 | Nyxia | 原初棱镜 | 根光路与诊断基底 | 光路发布、镜座控制 | 静态，不运行期替换 |
| 2 | Ignis | 光圈调焦环 | 候选与当前装镜画面 | 验证、装入、换代、拔出 | R1，由 Nyxia 最终发布 |
| 3 | Lemon | 光纤波导 | 光纤积压与 cursor | 有界 frame/cursor 传递 | R1 bridge + drain |
| 4 | Iris | 跨界折射镜 | 外部工具与数据源画面 | 外部协议调用与入射归一化 | R1 reconnect |
| 5 | Rhea | 神谕聚焦镜 | 模型目录、限制与价格画面 | 真实模型调用和流式结果 | R1 in-flight drain |
| 6 | Janus | 双面反射镜 | 当前直连 Agent 画面 | 产生下一模型 Act 或停止 | R1 step boundary |
| 7 | Clotho | 光栅分束镜 | 显式工作流当前步 | 推进确定性 DAG | R1 Photon rebuild |
| 8 | Aya | 分形复眼镜 | 子运行状态与合并候选 | fork、join、merge proposal | R1 child beam drain |
| 9 | Textus | 光谱滤波镜 | 对话、预算、压缩后的 ModelSurface | 产生压缩/摘要 Act | R1 differential view |
| 10 | Enso | 全息定影镜 | instruction、skill、memory、RAG | 检索与记忆候选 Act | R1 index rebuild |
| 11 | Techor | 光能作动镜 | 工具 schema 与 ActSurface | 动态萃取、规范化、目标匹配 | R1 route epoch swap |
| 12 | Styx | 暗室隔离镜 | sandbox strength 和限制 | 受限进程、PTY、终止 | R1 process drain |
| 13 | Fallen | 偏振滤光镜 | 风险、审批与拒绝理由 | 准入、阻断、审批 | R1 dual evaluation |
| 14 | Cista | 遮光秘盒 | SecretRef 与脱敏画面 | 短时绑定和出口清洗 | R1 binding expiry |
| 15 | Chora | 光感底片 | 存储 tail、segment、健康度 | 唯一 Photon append 与 blob 写入 | R1 writer handoff |
| 16 | Tracket | 光路记录镜 | 因果图与回放画面 | schema、因果和 hash 验证 | R1 shadow replay |
| 17 | Nota | 光谱分析仪 | metric、trace、profile、doctor | 遥测导出与诊断包 | R1 exporter overlap |
| 18 | Cove | 实景物镜 | 文件树、Git、Diff、artifact | 文件/Git/工作区 Act | R1 rescan + watcher handoff |
| 19 | Snow | 纯白投影幕 | CLI、本地协议、headless 画面 | CLI/RPC 输入折射 | R1 listener handoff |
| 20 | Termon | 全息显像屏 | Slint Workbench 画面 | 人类输入与审批折射 | R2 desktop handoff |

二十个 Lens 使用同一个 `view/refract` 契约，但它们观察的 PhotonPattern、贡献的 SurfaceChannel 和接受的 ActPattern 不同。Nyxia 的实现静态内建，其他十九个都由 artifact generation 承载。

---

## 12. 二十透镜详细设计

### 12.1 Nyxia：原初棱镜 / 元框架微内核

**定位**：唯一静态内建的光学定律与光具座。

**观察**：bootstrap Photon、mount epoch、engine 状态、镜座活动和停止信号。  
**显像**：`DiagnosticSurface` 的 LightPath、generation、Beam、guard、queue 和故障信息。  
**折射**：`mount.commit`、`mount.unmount`、`system.stop`、`system.handoff`。  
**硬约束**：

- 只有 Nyxia 发布 `LightPathSnapshot`；
- 只有 Nyxia 签发 BeamTicket；
- 动态代码只能通过 ABI adapter 或 worker bridge；
- 所有镜座活动可枚举、可停止、有 deadline；
- Nyxia 不实现任何业务 tool 或 UI 页面。

**验收**：随机装镜、换代、停止、崩溃序列下，不出现新光束进入旧 generation，不出现未归属镜座的线程、句柄或子进程。

### 12.2 Ignis：光圈调焦环 / 镜片编排

**观察**：desired light-path Photon、artifact catalog、trust 结果、dark-lane 报告和当前 epoch。  
**显像**：候选版本、差异、permission 变化、验证进度、afterglow。  
**折射**：`lens.verify`、`lens.mount`、`lens.replace`、`lens.unmount`、rollback-as-new-epoch。  
**规则**：

- manifest/order/pattern 冲突在发布前解决；
- rollback 也是一份新 LightPath，不改旧 epoch；
- Ignis 不能自行发布，最终 CAS 由 Nyxia 执行；
- 候选失败不污染 active LightPath。

**换代**：当前 Ignis 准备候选，Nyxia 以固定 builtin adapter 完成原子换代。

### 12.3 Lemon：光纤波导 / 有界传输

**观察**：cursor、frame、consumer lag、queue capacity。  
**显像**：带宽、积压、丢弃策略、追赶状态。  
**折射**：`waveguide.send-frame`、`waveguide.advance-cursor`、`waveguide.reconnect`。  
**规则**：

- 传递 typed frame、Photon cursor 和流式 chunk，不建立全局事件总线；
- 所有 conduit 有容量和背压策略；
- durable consumer 只持 cursor，掉线后从 Chora 追赶；
- UI/model token 可合批，但不能改变 committed Photon 顺序。

### 12.4 Iris：跨界折射镜 / 外部协议桥

**观察**：MCP、LSP、本地扩展协议的远端描述和连接状态。  
**显像**：归一化的 tool schema、source schema、server health。  
**折射**：外部 tool call、source poll/subscribe、连接、断开和重试。  
**规则**：

- 外部 schema 映射为本地 schema id；
- 远端返回只形成 PhotonDraft；
- 网络写使用 idempotency key；
- 远端不可信文本不能成为 system instruction；
- 断线、超时和结果未知分开记录。

### 12.5 Rhea：神谕聚焦镜 / 模型网关

**观察**：ModelSurface、模型目录、凭据引用、限额、路由配置。  
**显像**：可选模型、token/window 限制、估算成本、当前健康度。  
**折射**：`model.call`，真实发送 provider 请求并追加 dispatched/chunk/message/usage/failed Photon。  
**规则**：

- 发送前的 ModelSurface 摘要、tool schema hash、模型 id 和 epoch 先提交；
- streaming chunk 可合批提交，但最终 message 与 usage 必须完整；
- provider 重试必须遵守幂等与预算；
- 模型文本不直接执行，structured call 交给 Techor；
- 凭据只通过 Cista 短时绑定。

### 12.6 Janus：双面反射镜 / 默认 Agent 光路

**观察**：用户输入、模型响应、Act 结果、预算和 stop Photon。  
**显像**：当前 step、下一次是否需要模型、结束原因。  
**折射**：提出下一次 `model.call` Act，或追加 `ray.darkened` 建议。  
**规则**：

- Janus 是唯一默认 direct loop；
- 不暗中启动 Clotho 或 Aya；
- 无新 Act 且无等待项时自然停机；
- 重复相同 Act 触发振荡检测；
- 所有决策可由 Photon 重放。

### 12.7 Clotho：光栅分束镜 / 显式工作流

**观察**：workflow definition Photon、step result、branch/join 条件。  
**显像**：DAG、当前节点、依赖完成度、失败策略。  
**折射**：提出下一个确定性 step Act、retry Act 或 workflow completion。  
**规则**：只有显式 workflow 才启用；定义版本和输入 hash 固定；条件判断有 schema；补跑产生新 Photon。

### 12.8 Aya：分形复眼镜 / 子运行

**观察**：`child.spawn` Act、父子 ray、预算、workspace mode、join policy。  
**显像**：子运行卡片、进度、成本、输出摘要、合并候选。  
**折射**：创建新 stream、启动 child ray、等待结果、提出 Cove merge Act。  
**规则**：

- 子运行拥有独立 stream；
- 预算和允许的 ActKind 不能超过父运行给定上界；
- 默认工作区只读，写入使用独立 worktree；
- join 只把 summary/reference 追加父流；
- 文件合并必须经过新的 Cove Act。

### 12.9 Textus：光谱滤波镜 / ModelSurface

**观察**：对话、tool result、summary、model budget、当前 light-path epoch。  
**显像**：system fragments、messages、tool-visible history、token estimate。  
**折射**：`text.compact`、`text.summarize` Act，结果以 summary Photon 返回。  
**规则**：

- 最终 Prompt 不持久化为规范状态；
- 历史中已拔出的 tool 调用只显示为不可执行叙述；
- tool schema 只来自当前 LightPath 的 `view`；
- 截断顺序、预算和降级原因确定；
- 缓存键至少含 tail seq、epoch、model id 和 reducer version。

### 12.10 Enso：全息定影镜 / Instruction、Skill、Memory、RAG

**观察**：instruction source、skill artifact、memory Photon、RAG document/index version。  
**显像**：经过来源标记和预算裁剪的 ModelSurface contribution。  
**折射**：检索 Act、memory proposal、index rebuild Act。  
**规则**：

- retrieval result 带 document id、chunk id、hash 和来源；
- 外部内容默认 data，不升级为 instruction；
- memory 写入需要显式 policy；
- index 是派生文件，可从原文和 Photon 重建；
- 拔出 Enso 后，其 contribution 下一次折叠立即消失。

### 12.11 Techor：光能作动镜 / Tool 与 Code Mode

**观察**：当前 tool schemas、model.tool-call Photon、code-mode frame、Act result。  
**显像**：ModelSurface tool definitions、ActSurface 候选动作和解析错误。  
**折射**：schema 解码、Act 规范化、目标 pattern 匹配、结果预算化。  
**规则**：

- 不解析自由文本命令标签作为生产主路径；
- `(kind, schema)` 必须唯一命中；
- unknown tool、旧 epoch tool、schema mismatch 一律拒绝；
- Code Mode 仍产出结构化 Act，不获得安全旁路；
- 大结果存 artifact，Photon 只含摘要和引用。

### 12.12 Styx：暗室隔离镜 / 执行隔离

**观察**：已准入 Act、platform policy、sandbox backend、限制。  
**显像**：真实 `SandboxStrength`、网络/文件权限、deadline、资源上限。  
**折射**：process exec、PTY、worker、WASM、终止进程树。  
**规则**：

- 平台隔离不可用时不能静默降级；
- stdout/stderr 使用有界 ring 并流式形成 Photon；
- cancel 先 graceful，超时后 terminate process tree；
- 工作目录必须经 Cove canonicalize；
- secret 注入只在 Cista 批准的最终边界发生。

### 12.13 Fallen：偏振滤光镜 / Policy 与审批

**观察**：Act、risk rules、用户决定、信任来源、workspace 状态。  
**显像**：允许、拒绝、需审批、风险解释和最小权限差异。  
**折射**：`act.admitted`、`act.denied`、`approval.requested`、审批超时。  
**规则**：

- deny 优先；
- approval 绑定规范化后的 Act hash、epoch 和 deadline；
- 参数变化使旧批准失效；
- policy 错误默认拒绝；
- UI 只展示结果，不能绕过 Fallen 改写准入。

### 12.14 Cista：遮光秘盒 / Secret 与脱敏

**观察**：SecretRef、Act 目标、用途、出口类型。  
**显像**：只显示引用、来源和可用性，不显示明文。  
**折射**：最终边界短时绑定、输出脱敏、敏感字段清洗。  
**规则**：

- 明文不进入 Photon、ModelSurface、普通日志、UI 或 crash bundle；
- 绑定只对特定 Act、目标和 deadline 有效；
- 候选 generation 不继承旧绑定；
- redaction 失败阻止 commit 或模型发送。

### 12.15 Chora：光感底片 / 持久化

**观察**：PhotonDraft、blob、tail、segment、磁盘健康。  
**显像**：stream tail、容量、checkpoint、归档和完整性状态。  
**折射**：唯一 append transaction、blob put、checkpoint build、immutable archive。  
**规则**：

- 单 writer 分配 seq；
- committed table 禁止 update/delete；
- append 后才发 cursor 通知；
- writer 换代采用短 commit barrier 和 writer-token handoff；
- checkpoint 不是事实源；
- 备份必须包含 DB、blob、schema 和 mount epoch。

### 12.16 Tracket：光路记录镜 / 因果与回放

**观察**：所有 PhotonDraft、schema bundle、parent、hash chain。  
**显像**：timeline、DAG、Act 因果链、R0–R3 回放。  
**折射**：验证、拒绝错误 draft、创建 replay/fork proposal。  
**规则**：

- schema、父节点、stream、seq、epoch、payload hash 都要验证；
- golden ray 比较语义输出而非不稳定时间；
- 回放不执行现实动作，除非明确选择 R3 新 fork；
- 轨迹导出默认经过 Cista 清洗。

### 12.17 Nota：光谱分析仪 / 可观测性

**观察**：engine step、view duration、Beam、queue、DB、UI projection。  
**显像**：metric、trace、profile、health、doctor 和 crash summary。  
**折射**：telemetry export、profile capture、diagnostic bundle Act。  
**规则**：

- 遥测不是恢复源；
- correlation 至少包含 ray/step/photon/act/beam/lens/generation/epoch；
- exporter 失败不能阻塞 Photon commit；
- payload 和 secret 默认不进入 span；
- 诊断包生成经过 Fallen 与 Cista。

### 12.18 Cove：实景物镜 / Workspace、Git、Artifact

**观察**：workspace root、文件树、watch event、Git index/worktree、artifact。  
**显像**：canonical path、文件树、status、Diff、artifact preview。  
**折射**：read/write/move/delete、Git stage/commit/branch/merge、artifact create。  
**规则**：

- 所有路径先 canonicalize，再验证仍处于允许根目录；
- symlink/reparse point 穿越必须拒绝或显式批准；
- 写前记录 intent 和 precondition hash；
- 写后重新读取、hash 并追加 `fs.changed`；
- Diff 是观察结果，不是假定执行成功的证据；
- destructive Act 默认需要审批。

### 12.19 Snow：纯白投影幕 / CLI 与本地协议

**观察**：CliSurface、UiSurface delta、protocol cursor、daemon health。  
**显像**：人类 CLI、JSON Lines、desktop snapshot/delta。  
**折射**：CLI 命令、local RPC request、desktop reconnect、cancel。  
**规则**：

- machine output 与 human output 分离；
- protocol frame 有长度上限、版本、request id、cursor；
- 同一用户默认使用本地 socket/pipe 身份校验；
- 重连先 snapshot 后 delta；
- Snow 不拥有 Agent 业务状态。

### 12.20 Termon：全息显像屏 / Slint 桌面端

**观察**：Snow 提供的 committed Photon snapshot/delta 和连接状态。  
**显像**：对话、计划、审批、执行、Diff、终端、artifact、历史、镜片与诊断页面。  
**折射**：用户输入、取消、审批决定、打开 artifact、换镜请求。  
**规则**：

- Slint 属性是显像缓存，不是事实源；
- UI event 先转结构化 intent，再由 daemon 追加 Photon；
- desktop crash 不影响 daemon ray；
- 重启后按 cursor 重建；
- Termon 换代由 launcher 启动候选窗口、同步 cursor、原子切换本地 endpoint。

---

## 13. 端到端光流

### 13.1 普通对话

```text
1. Termon.refract(user submit)
2. Snow receives ui.intent frame
3. Tracket validates → Cista cleans → Chora appends user.input P1
4. RayTracingEngine begins step S1
5. Textus/Enso/current lenses view(P1) → ModelSurface
6. Janus proposes model.call A1; A1 intent is appended
7. Fallen/Cista admit A1
8. Rhea.refract(A1) performs real provider call
9. model.chunk/message/usage Photons append
10. Techor finds no tool Act
11. ray.darkened appends
12. Termon view fold displays final response
```

### 13.2 工具调用

```text
model.tool-call(name=calculate, schema=v1, args={expression:"128*4"})
→ Techor generated codec decodes CalculateArgs
→ append act.proposed
→ Fallen admission
→ current LightPath matches CalculatorLens generation G
→ Nyxia issues BeamTicket(G)
→ CalculatorLens.refract performs real parser calculation
→ PhotonEmitter appends tool.result(512)
→ next engine step rebuilds ModelSurface
→ Rhea produces natural-language answer
→ no new Act, ray darkens
```

### 13.3 文件修改与审批

```text
model proposes fs.write
→ Techor normalizes path and payload hash
→ Fallen requests approval
→ Chora appends approval.requested
→ Termon displays exact path/Diff/risk
→ user decision becomes approval.decided Photon
→ Cista checks sensitive data
→ Styx/Cove execute with precondition hash
→ Cove rereads disk and computes Git Diff
→ fs.changed + git.diff + act.succeeded append
→ Termon displays committed outcome
```

拒绝不会删除 Act proposal；它追加 `act.denied`。对已发生写入的“撤回”必须是新的反向文件 Act，而不是编辑旧 Photon。

### 13.4 运行时换镜

```text
Ignis observes desired generation G2
→ verifies signature/schema/permission delta
→ dark-lane view replay and synthetic refract test
→ Fallen approves mount.commit Act when required
→ Chora appends mount epoch E2
→ Nyxia publishes LightPath E2
→ new view/Act use G2
→ G1 completes bounded afterglow
→ guard stops G1 and appends afterglow-ended
```

### 13.5 daemon 崩溃恢复

```text
launcher restarts tokmond
→ Chora opens WAL and validates tail
→ Tracket verifies hash chain
→ read last complete mount epoch
→ rebuild LightPath
→ restore active rays from ray.* Photons
→ any started Act without terminal Photon becomes outcome-unknown
→ idempotent Act may be reconciled; others require human decision
→ Snow and Termon reconnect by cursor
```

### 13.6 子运行

父 ray 提出 `child.spawn` Act；Aya 创建新 stream 和可选 worktree；child 独立推进。完成后只把 summary/artifact reference 追加父流。合并文件必须新的 Cove merge Act，冲突不自动覆盖。

---

## 14. Act Plane：现实动作管线

### 14.1 ImpactClass

```cpp
enum class ImpactClass : std::uint8_t {
    ObserveOnly,
    LocalWrite,
    LocalDestructive,
    NetworkRead,
    NetworkWrite,
    CredentialUse,
    ExternalIrreversible
};
```

风险分类不承诺现实可恢复。`ExternalIrreversible` 永远不能包装成可撤销操作。

### 14.2 固定管线

```text
ActDraft
→ Techor normalize + schema validate
→ append act.proposed
→ Fallen policy + approval
→ Cista secret binding/redaction plan
→ Styx execution envelope
→ target Lens.refract
→ observe and verify outcome
→ append terminal Photon
→ Tracket causal validation
```

每一步都只能收紧 Act，不能扩大 path、network、secret、deadline 或 impact 权限。

### 14.3 Admission

```cpp
struct Admission {
    AdmissionDecision decision; // Allow, Deny, NeedApproval
    std::vector<std::string> reasons;
    Hash normalized_act_hash;
    Deadline expires_at;
    ExecutionEnvelope envelope;
};
```

approval 必须绑定 `normalized_act_hash + light_path_epoch + target_generation`。任何参数、目标或 generation 变化都使批准失效。

### 14.4 幂等、补偿与结果未知

- 可重试 Act 必须有稳定 idempotency key；
- 执行前 append intent，执行后 append observation；
- 网络断开不能推断远端未执行；
- `outcome-unknown` 需要 provider query、人工核对或补偿 Act；
- compensation 是新的现实动作，有自己的风险和结果；
- UI 不使用“已撤销”描述旧 Act，只展示后续补偿事实。

### 14.5 Cancel

Cancel 是新的 `ray.cancel-requested` 或 `act.cancel-requested` Photon。它要求 Nyxia 向对应 BeamTicket 发 stop，但不能删除已经产生的 Photon，也不能保证外部系统接受取消。

---

## 15. 本地协议、配置与目录

### 15.1 Snow protocol

传输：Windows named pipe；macOS/Linux Unix domain socket。生产默认同一用户访问。

```cpp
struct FrameHeader {
    std::uint32_t magic;
    std::uint16_t protocol_major;
    std::uint16_t protocol_minor;
    std::uint32_t flags;
    std::uint32_t payload_size;
    std::uint64_t request_id;
    std::uint64_t cursor;
};
```

Frame payload 使用 canonical CBOR。核心消息：

| 方向 | 消息 |
| --- | --- |
| client → daemon | `hello`, `snapshot.request`, `intent.submit`, `approval.decide`, `ray.cancel`, `lens.command`, `artifact.open` |
| daemon → client | `welcome`, `snapshot`, `photon.delta`, `surface.delta`, `approval`, `artifact.meta`, `health`, `error` |

规则：

- major 不兼容立即拒绝；minor 只做向后兼容字段扩展；
- payload 长度、嵌套深度、字符串和数组元素数有限制；
- request id 可重试，daemon 按幂等表去重；
- cursor gap 触发 snapshot，不猜测缺失 delta；
- desktop 不能发送 committed Photon，只能发送 intent。

### 15.2 配置折叠

Tokmon 自己定义、由人维护的运行配置、信任配置、bootstrap lock 和 Lens manifest 全部使用 YAML。JSON 只保留给外部规范强制的文件，例如 JSON Schema、`CMakePresets.json` 和 SPDX/CycloneDX JSON；这些文件不是 Tokmon 运行配置。

```text
built-in defaults
→ signed machine config
→ user config: <user-home>/.tokmon/config.yaml
→ project config: <workspace>/.tokmon/config.yaml
→ session config Photon
→ command-line override Photon
```

配置选择结果追加 `config.selected`，包含用户目录、项目目录、每个输入文件的内容 hash 和最终配置 hash；secret 只写 `SecretRef`。

合并规则：

- map 递归合并，scalar 由后一级覆盖；
- LightPath 条目按 `lens_id` 合并，不按数组位置猜测身份；
- 项目级配置可以收紧风险、网络、文件和 SecretRef 使用范围，不能扩大用户级信任边界；
- 用户级 `trust.yaml` 是信任根来源，项目目录不能新增根签名者；
- `<workspace>/.tokmon/local.yaml` 只保存本机覆盖并必须加入 `.gitignore`；
- YAML 未知字段、重复 key、类型不匹配和非法路径全部返回 `tl::expected` 错误，不静默采用默认值。

```yaml
runtime:
  max_active_rays: 8
  max_steps_per_ray: 64
  max_parallel_beams: 16
  afterglow_deadline_ms: 5000

photons:
  max_inline_payload_bytes: 65536
  view_window_photons: 10000
  verify_hash_tail: 4096

lenses:
  require_signatures: true
  unknown_native_mode: worker

ui:
  stream_batch_ms: 16
  max_terminal_buffer_bytes: 8388608
```

### 15.3 用户级 `.tokmon` 目录

Tokmon 不再把用户配置放入无点前缀的 `tokmon/` 目录。所有平台统一使用用户主目录下的 `.tokmon`：

```text
Windows: %USERPROFILE%\.tokmon\
macOS:   $HOME/.tokmon/
Linux:   $HOME/.tokmon/
```

逻辑布局：

```text
.tokmon/
├─ config.yaml
├─ trust.yaml
├─ light-path.yaml
├─ lens-lock.yaml
├─ data/
│  ├─ photons.sqlite3
│  ├─ blobs/
│  ├─ segments/
│  ├─ checkpoints/
│  └─ artifacts/
├─ lenses/
│  ├─ installed/
│  └─ quarantine/
├─ runtimes/
│  ├─ node/<exact-version>/<platform-arch>/
│  ├─ cpython/<exact-version>/<platform-arch>/
│  └─ environments/<artifact-hash>/
├─ cache/
│  ├─ projection/
│  └─ ui/
├─ logs/
└─ run/
```

`runtimes/` 保存经校验的 Node.js/CPython 精确发行版以及按 artifact hash 物化的不可变依赖环境。兼容 Lens 可以共享只读 runtime，但不共享可写 `node_modules`、`site-packages` 或虚拟环境。runtime 与 environment 都由 Ignis 在 dark lane 中按需准备；`tokmond` 不链接、嵌入或在地址空间内初始化 Node.js/CPython。

权限：整个用户级 `.tokmon` 默认仅当前用户可读写；socket/pipe 验证用户身份；secret 存 OS keyring，不在 YAML 中保存明文。

### 15.4 项目级 `.tokmon` 目录

每个工作区可以有自己的控制目录：

```text
<workspace>/
├─ .tokmon/
│  ├─ config.yaml
│  ├─ light-path.yaml
│  ├─ lens-lock.yaml
│  ├─ policy.yaml
│  ├─ local.yaml             # 必须 gitignore
│  ├─ instructions/
│  └─ skills/
└─ project files...
```

- `config.yaml`：项目模型、预算、UI 和工作流默认值；
- `light-path.yaml`：项目希望装入、禁用或换代的 Lens；
- `lens-lock.yaml`：精确 artifact hash、schema hash、版本、签名者、语言 runtime 与依赖树 hash；
- `policy.yaml`：项目级风险收紧规则；
- `instructions/`、`skills/`：由 Enso 作为数据观察并投影视界；
- `local.yaml`：开发者本机覆盖，不得提交；
- 项目目录不保存用户级 Photon DB、全局日志、secret 明文或信任私钥。

从陌生仓库打开项目时，`.tokmon` 只是未经信任的候选配置。它不能自行安装 artifact、下载 Node.js/CPython、执行 `npm install`/`pip install`、增加信任根、扩大 Act 权限或绑定 secret；相关变化必须经过 artifact 验证和 Fallen 审批。

---

## 16. Termon：Slint 桌面显像

### 16.1 版本与集成

截至本文基线，Slint 稳定版本固定为 `v1.17.1`。发布构建使用经过审计的 C++ SDK；开发环境可用 FetchContent，但仍固定 tag。

```cmake
find_package(Slint 1.17.1 EXACT CONFIG REQUIRED)

add_executable(tokmon-desktop
  apps/tokmon-desktop/main.cpp
  lenses/termon/src/termon_controller.cpp
  lenses/termon/src/ui_projection.cpp)

target_compile_features(tokmon-desktop PRIVATE cxx_std_20)
target_link_libraries(tokmon-desktop PRIVATE Slint::Slint tokmon_protocol)

slint_target_sources(tokmon-desktop
  lenses/termon/ui/app.slint
  LIBRARY_PATHS "tokmon=lenses/termon/ui")
```

```cmake
include(FetchContent)
FetchContent_Declare(
  Slint
  GIT_REPOSITORY https://github.com/slint-ui/slint.git
  GIT_TAG v1.17.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR api/cpp)
FetchContent_MakeAvailable(Slint)
```

Termon 生产路径使用 AOT `.slint` 编译。运行期解释只允许开发预览，不成为第二条产品 UI 装配路径。UI 换代由进程 handoff 完成。

### 16.2 UI 即 Lens

| Termon 行为 | 透镜语义 |
| --- | --- |
| snapshot/delta → model | PhotonWindow |
| C++ projection → Slint property/model | `view` |
| 点击/输入/审批 | intent |
| intent → Snow request | `refract` |
| daemon committed delta | new Fact |

Slint model 是 `UiSurface` 的显存缓存。断线、窗口关闭或 desktop 崩溃不会改变 daemon 中的 ray。

### 16.3 `.slint` contract

```slint
import { Button, LineEdit, ListView, VerticalBox } from "std-widgets.slint";
import { ConversationItem } from "@tokmon/conversation-item.slint";

export struct ConversationRow {
    id: string,
    kind: string,
    title: string,
    body: styled-text,
    status: string,
    source-seq: int,
}

export component AppWindow inherits Window {
    in property <[ConversationRow]> conversation;
    in property <string> connection-state;
    in property <bool> ray-active;

    callback submit-message(string);
    callback cancel-ray();
    callback decide-approval(string, bool);
    callback request-lens-command(string, string);
    callback open-artifact(string);

    VerticalBox {
        ListView {
            for row in root.conversation : ConversationItem { item: row; }
        }

        composer := LineEdit {
            enabled: !root.ray-active;
            accepted(text) => { root.submit-message(text); }
        }

        Button {
            text: root.ray-active ? "停止" : "发送";
            clicked => {
                if (root.ray-active) { root.cancel-ray(); }
                else { root.submit-message(composer.text); }
            }
        }
    }
}
```

真实 UI 拆分为 token、component、panel、screen library；根组件只导出稳定 property/callback contract。

### 16.4 C++ controller 与线程边界

```cpp
class TermonController final {
public:
    using AppHandle = slint::ComponentHandle<AppWindow>;

    explicit TermonController(const AppHandle& ui) : weak_ui_(ui) {}

    void bind(const AppHandle& ui) {
        ui->on_submit_message([this](slint::SharedString text) {
            snow_.submit_intent(UiIntent::user_input(std::string{text}));
        });
        ui->on_cancel_ray([this] { snow_.submit_intent(UiIntent::cancel_ray()); });
    }

    void on_photon_batch(UiBatch batch) {
        auto weak = weak_ui_;
        slint::invoke_from_event_loop(
            [weak, batch = std::move(batch)]() mutable {
                if (auto ui = weak.lock()) {
                    apply_batch_to_models(*ui, std::move(batch));
                }
            });
    }

private:
    slint::ComponentWeakHandle<AppWindow> weak_ui_;
    SnowClient snow_;
};
```

`TermonController` 生命周期覆盖已注册 callback。任何需要再次访问 UI 的后台 closure 只捕获 weak handle。UI 线程禁止 DB、Git、network、模型调用和 heavy Markdown parse，也禁止调用 blocking event-loop variant。

### 16.5 状态分层

```cpp
struct CoreProjection {
    std::vector<SessionSummary> sessions;
    std::optional<RayProjection> active;
    std::vector<ApprovalView> approvals;
    LightPathView light_path;
    DurableCursor cursor;
    std::uint64_t revision{};
};

struct UiEphemera {
    std::string composer_draft;
    std::optional<std::string> selected_panel;
    float scroll_offset{};
    bool sidebar_open{};
};
```

`CoreProjection` 可由 snapshot/delta 重建；`UiEphemera` 可以本地保存，但不能影响 daemon 决策。

### 16.6 Model 与流式更新

- 会话、对话、轨迹、Diff hunk、terminal line 使用 `slint::Model`；
- 大列表使用 `ListView` 虚拟化，不创建完整 widget tree；
- token chunk 在 worker 线程合并为 8–16 ms batch；
- 每批只执行一次 `invoke_from_event_loop`；
- completed Markdown 使用 `styled-text`/预解析缓存；
- terminal 使用 byte ring 和可见行索引；
- 10k trajectory 节点按窗口加载。

### 16.7 P0 页面

1. Workspace/session sidebar；
2. conversation timeline；
3. composer 与附件；
4. plan/workflow；
5. approval drawer；
6. tool execution card；
7. terminal；
8. Diff/file preview；
9. completion summary；
10. reconnect/recovery；
11. history/replay；
12. Lens manager 与 LightPath inspector；
13. settings/doctor。

### 16.8 Design token、无障碍与本地化

- color/spacing/radius/type/motion 只从 `.slint` token global 读取；
- light/dark/high-contrast 有独立截图基线；
- 完整键盘导航、焦点环和 screen-reader label；
- CJK、emoji、RTL、IME、125%–250% DPI 有测试；
- 流式更新尊重 reduced motion；
- 错误、风险与状态不能只靠颜色区分。

### 16.9 Slint 许可证

发行前由法务/负责人选择适用的官方许可路径，完成 attribution、第三方 NOTICE、SBOM 和二进制审计。版本升级必须重新复核许可证和依赖清单。

---

## 17. C++20 宿主工程与多语言 Lens 构建

### 17.1 工具链

```cmake
cmake_minimum_required(VERSION 3.25)
project(tokmon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

建议矩阵：

| 平台 | 编译器 | 生成器 |
| --- | --- | --- |
| Windows | MSVC 2022 | Ninja / Ninja Multi-Config |
| macOS | AppleClang | Ninja / Xcode CI smoke |
| Linux | GCC/Clang | Ninja |

CI 开启 warnings-as-errors、ASan/UBSan、TSan 专项、clang-tidy、format、dependency audit 和 reproducible package 检查。

### 17.2 依赖

| 类别 | 建议 | 用途 |
| --- | --- | --- |
| UI | Slint 1.17.1 exact | Termon |
| async/network | Asio | C++20 coroutine、socket、timer |
| storage | SQLite | Photon、索引、checkpoint |
| crypto | libsodium/平台 API | hash、signature、secure memory |
| serialization | canonical CBOR + generated codec | Photon/Act/ABI/protocol |
| C++ error result | tl::expected | 宿主/native Lens 显式错误返回和 `Result<T>` |
| config | yaml-cpp | YAML 配置与 Lens manifest |
| C++ logging | spdlog | 宿主进程的唯一结构化日志实现 |
| testing | Catch2 + RapidCheck | unit/property |
| compression | zstd | immutable segment/artifact |

具体版本写入 lockfile、SBOM 和 artifact provenance，不在源码中跟随移动版本。

```cmake
find_package(tl-expected CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

target_link_libraries(tokmon_base
  PUBLIC
    tl::expected
    spdlog::spdlog
    yaml-cpp::yaml-cpp)
```

- C++ `Result<T>` 统一定义为 `tl::expected<T, Error>`，禁止 C++ Lens 自建不兼容错误容器；
- `spdlog` 是 daemon、CLI、C++ worker host、desktop 和 launcher 的唯一日志实现；Node.js/CPython Lens 的日志通过 `log.write` frame 交给 host，再由 `spdlog` 写出；
- YAML 解析结果先经 schema/字段白名单校验，再转换成不可变运行配置；
- YAML 未知字段默认报错，禁止因拼写错误静默回退到默认值。

Node.js/CPython 不作为 CMake 链接依赖，而作为受签名与 hash 约束的 Lens runtime：

- Node.js 锁定 exact release、platform、architecture；SDK 发布为 `@tokmon/lens-sdk`，TypeScript 构建只产出可执行 ESM；
- CPython 锁定 major/minor/patch、platform、architecture 与 extension ABI；SDK 发布为 `tokmon-lens-sdk` wheel；
- runtime 发行包、SDK、dependency bundle/wheel bundle、lockfile、SBOM、许可证和校验值进入 artifact provenance；
- CI 构建阶段允许从批准 registry 解析依赖，发布后装镜和运行阶段一律使用离线、只读、已验证内容；
- runtime 安全更新形成新的 runtime hash 和 mount epoch，不原地修改正在运行的 environment。

### 17.3 仓库结构

```text
tokmon-n/
├─ CMakeLists.txt
├─ CMakePresets.json
├─ cmake/
├─ docs/
│  └─ DESIGN.md
├─ apps/
│  ├─ tokmond/
│  ├─ tokmon-launcher/
│  ├─ tokmon-desktop/
│  ├─ tokmon-cli/
│  └─ tokmon-lens-worker/
│     ├─ native/
│     ├─ node/
│     └─ cpython/
├─ nyxia/
│  ├─ engine/
│  ├─ light_path/
│  ├─ mount/
│  ├─ host/
│  ├─ loader/
│  └─ worker/
├─ sdk/
│  ├─ cpp/
│  ├─ c/
│  ├─ typescript/
│  ├─ python/
│  ├─ worker-protocol/
│  ├─ schemas/
│  ├─ codegen/
│  └─ testkit/
├─ lenses/
│  ├─ ignis/
│  ├─ lemon/
│  ├─ iris/
│  ├─ rhea/
│  ├─ janus/
│  ├─ clotho/
│  ├─ aya/
│  ├─ textus/
│  ├─ enso/
│  ├─ techor/
│  ├─ styx/
│  ├─ fallen/
│  ├─ cista/
│  ├─ chora/
│  ├─ tracket/
│  ├─ nota/
│  ├─ cove/
│  ├─ snow/
│  └─ termon/
├─ protocol/
├─ schemas/
├─ tests/
│  ├─ unit/
│  ├─ property/
│  ├─ golden-rays/
│  ├─ crash-matrix/
│  ├─ abi/
│  ├─ security/
│  └─ ui/
└─ packaging/
```

### 17.4 Target 规则

- `tokmon_nyxia` 不链接具体业务 Lens；
- `tokmon_lens_sdk` 不依赖 daemon 内部头文件；
- 每个 Lens 单独 target、manifest、schema bundle 和 contract test；
- `tokmon-desktop` 是唯一链接 Slint 的主产物；
- C ABI header 单独安装并保持自包含；
- generated schema code 纳入 deterministic build 检查；
- worker 与 daemon 之间只使用 protocol target；
- Node.js/CPython adapter 不链接进 `tokmond`，只存在于 `tokmon-lens-worker` 进程；
- TypeScript/Python SDK 由同一 schema 与 Worker Protocol 定义生成，并运行跨语言 golden contract；
- Node dependency bundle 与 Python wheel environment 都是按 artifact hash 生成的不可变产物；
- test double 作为测试 Lens artifact，不进入生产二进制。

### 17.5 产物

```text
tokmond
tokmon
tokmon-launcher
tokmon-desktop
tokmon-lens-worker
libtokmon-lens-sdk
tokmon-lens-sdk headers/codegen/testkit
@tokmon/lens-sdk package
tokmon-lens-sdk Python wheel
Node.js/CPython worker adapters and verified runtime catalog
lens artifacts + schema bundles + signatures + SBOM
platform installers
```

---

## 18. 安全与供应链

### 18.1 信任层级

| 等级 | 代码 | 默认运行位置 | 允许范围 |
| --- | --- | --- | --- |
| T0 | Nyxia/launcher | 宿主 | 最小光学定律与启动 |
| T1 | 官方签名 Lens | daemon 或专用进程 | manifest 声明的窄权限 |
| T2 | 已批准第三方 Lens | native/Node.js/CPython worker 或 WASM | 受限 host bridge |
| T3 | 未验证 artifact | quarantine | 只 inspect/verify，不执行 |

信任等级不由 Lens 自报；由 artifact signature、来源、用户策略和安装记录共同决定。

### 18.2 不可绕过边界

- Photon 只能经 Tracket → Cista → Chora 提交；
- Act 只能经 Techor → Fallen → Cista → Styx → target 执行；
- worker 不能直接打开 Photon DB；
- worker 不能取得任意文件系统和网络；
- Node.js/CPython 的语言级限制不作为安全边界，最终边界始终是 Styx、OS sandbox 与受限 OpticalHost；
- worker 不继承用户 shell 的 preload、模块搜索路径、包管理器配置、代理、credential 或无关环境变量；
- dynamic artifact 不能改变 manifest 后再继续使用原签名；
- UI、CLI、MCP 和模型都只能提出 intent，不能伪造 terminal Photon；
- Code Mode 与 shell 仍受相同 Act gate；
- secret 明文只在最终绑定边界短时存在；
- path canonicalization 在每次文件 Act 执行前重新验证。

### 18.3 Artifact 验证

安装/装镜前验证：

1. 内容 hash；
2. signature chain 与 trust policy；
3. manifest/schema/binary 一致性；
4. C ABI 或 Worker Protocol major/minor；
5. platform/arch/toolchain、Node.js/CPython exact runtime 与扩展 ABI；
6. surface channel、ActPattern 与 permission 差异；
7. SBOM、许可证和已知漏洞；
8. dark-lane contract test；
9. runtime、SDK、lockfile、离线依赖树与其 hash 是否完全一致；
10. worker sandbox 能力；
11. artifact 是否被 quarantine。

任何失败追加 `lens.rejected`，不得以 debug flag 静默绕过生产策略。

### 18.4 Secret 数据流

```text
config/Photon/Surface: SecretRef only
→ Act admitted
→ Cista obtains secret from OS keyring
→ binds to exact target + purpose + deadline
→ injects at final network/process boundary
→ scrubs request diagnostics and output
→ zeroizes temporary buffer
```

禁止把完整环境变量、HTTP Authorization、cookie、private key、token 或 shell history 写入 Photon 和普通日志。

### 18.5 平台隔离

- Windows：Job Object、restricted token/AppContainer（适用时）、ACL、named pipe identity；
- macOS：sandbox profile、hardened runtime、code signing、Keychain；
- Linux：namespace、seccomp、cgroup、Landlock（可用时）、Unix credential；
- WASM：memory/fuel/table limit、显式 host imports；
- Node.js/CPython：独立进程、只读 artifact/environment、私有临时目录、清洗后的环境变量和专用 IPC；
- 所有平台都必须把实际 `SandboxStrength` 显示给 Fallen 和 UI。

### 18.6 供应链

- 所有依赖固定版本与校验值；
- 构建生成 SPDX/CycloneDX SBOM；
- release artifact 签名并生成 provenance；
- CI 扫描依赖漏洞、许可证、密钥和二进制导出符号；
- updater 先下载到 quarantine，验证后原子切换；
- 安装包支持回到旧二进制，但启动后仍以新的系统 Photon 记录切换；
- Lens artifact 与主程序采用独立签名和撤信列表；撤信只阻止后续加载，不改写历史。
- 装镜、启动和热切换阶段禁止访问公共 npm/PyPI registry；Node dependency bundle 与 Python wheel bundle 必须在发布阶段完成锁定、下载、扫描和签名；
- `package-lock.json`/等价精确 lock 与 Python hash-lock 必须覆盖全部传递依赖，物化后的 dependency tree hash 还要再次核对；
- Node native addon 与 CPython C extension 按 native code 对待：平台专包、符号/许可证/漏洞扫描、强制 worker，不因来自 npm/PyPI 而提高信任；
- Node.js/CPython runtime 自身进入漏洞清单与更新策略；安全升级通过新 artifact/runtime hash 和新 mount epoch 发布，旧环境只在 afterglow 窗口存活。

---

## 19. 诊断、性能与容量

### 19.1 结构化错误

```cpp
struct Error {
    ErrorCode code;
    std::string message;
    LensId lens;
    GenerationId generation;
    RayId ray;
    std::optional<ActId> act;
    std::optional<PhotonId> caused_by;
    bool retryable;
    std::vector<DiagnosticField> fields;
};
```

在 C++ 调用链中，`Error` 通过 `tl::expected<T, Error>` 显式传播；语言 worker 的 SDK `Result` 在协议边界转换为同一 `Error`。`spdlog` 只记录诊断副本，不参与错误控制流，也不能成为恢复依据。所有日志在进入 sink 前先经 Cista 脱敏，并附带可用的 photon/ray/act/beam/lens/generation/epoch 字段。

统一日志实现由 C++ `tokmon_logging` 薄封装约束，底层只允许 `spdlog`：launcher/CLI 默认 console sink，daemon/worker supervisor/desktop 使用 rotating file sink，并可由 Nota 增加受控 exporter sink。Node.js/CPython adapter 只发送结构化 `log.write`，不直接选择持久化 sink。异步日志队列必须有界；溢出策略不能阻塞 Photon append writer，warning/error 则同步写入应急 sink。

错误分类：validation、policy、secret、sandbox、I/O、provider、storage、ABI、worker crash、view nondeterminism、budget、cancel 和 outcome unknown。

### 19.2 LightPath inspector

`tokmon doctor --light-path` 和 Termon inspector 至少展示：

- 当前 epoch、path hash 和镜片顺序；
- Lens id/version/generation/artifact/signature；
- view channel、PhotonPattern、ActPattern；
- active Beam、afterglow deadline 和 stop 状态；
- guard 托管的 thread/timer/socket/process 数量；
- Lemon queue capacity/lag/drop policy；
- Chora tail、WAL、segment、checkpoint 和 disk pressure；
- 最近 view/refract 失败；
- worker sandbox strength 和 crash count。

诊断页面只读；换镜、终止和导出仍产生结构化 intent。

### 19.3 性能原则

- Photon append 使用批事务，但不改变逐 Photon seq；
- view 按 `(tail_seq, epoch, config_hash)` 做增量折叠；
- `PhotonWindow` 使用 span/arena/blob view，避免复制完整历史；
- Lemon 高频路径传 cursor 和小 frame；
- tool schema 编译后缓存，epoch 变化才重建；
- UI 每帧合并 delta，不逐 token 重建全模型；
- terminal、trajectory 和 Diff 使用窗口化数据结构；
- immutable segment 后台压缩，不阻塞 append writer；
- model/network/DB/worker 各自独立并有明确上限。

### 19.4 必须测量的指标

| 指标 | 分位 | 场景 |
| --- | --- | --- |
| Photon append latency | p50/p95/p99 | 1、16、128 batch |
| 20 Lens view fold | p50/p95/p99 | 1k/10k/100k history + checkpoint |
| engine step overhead | p50/p95/p99 | 不含模型/工具现实时间 |
| tool Act dispatch | p50/p95/p99 | schema decode + admission + match |
| Lens R1 swap | p50/p95/p99 | idle/in-flight/failure |
| desktop delta-to-paint | p50/p95/p99 | token/terminal/10k timeline |
| crash recovery | wall time | clean/WAL tail/worker crash |
| memory | steady/peak | long conversation、large terminal、multi-ray |

数字只有在固定硬件、数据集、构建类型和命令被记录后才能成为 release gate。

### 19.5 容量与退化

- 达到 Photon hot window 上限：使用 checkpoint + warm segment 读取；
- 达到 terminal byte 上限：把完整输出存 blob，UI 保留窗口；
- UI 消费落后：丢弃中间非规范 surface delta，按 cursor 请求新 snapshot；
- model chunk 过密：合批为 chunk Photon，保留最终 message；
- disk pressure：拒绝新的高风险 Act，保持只读 inspect/export；
- worker crash storm：quarantine generation，自动回到先前 LightPath 的新 epoch；
- hash 校验失败：进入 rescue，不继续执行 Act。

---

## 20. 测试策略

### 20.1 Lens Contract Suite

每个动态 Lens artifact 必须通过同一套测试：

1. manifest/schema/signature/ABI；
2. `view` 对相同输入确定；
3. `view` 不调用外部写 API；
4. `view` 只写声明过的 SurfaceChannel；
5. `refract` 只匹配声明过的 ActPattern；
6. schema invalid/oversized/unknown field 正确拒绝；
7. `refract` 只能经 PhotonEmitter 输出；
8. deadline/stop/output bound；
9. guard 停止后没有后台活动；
10. 拔镜后新 view 和新 Act 零贡献；
11. candidate dark-lane 失败不影响 active path；
12. crash 转换为结构化 Photon；
13. 用户级与项目级 `light-path.yaml` 合并结果确定；
14. 项目级配置不能扩大用户级信任边界；
15. YAML parse/schema 失败保持当前 LightPath。

### 20.2 属性测试

关键性质：

```text
committed photons never change
seq strictly increases per stream
hash chain verifies
view(Facts, Path) is deterministic
unmount(L) removes every future contribution from L
one Act pattern resolves to at most one active target
new step uses one immutable path epoch
denied Act never reaches target refract
no new Act + no pending Beam implies darkened
fork/replay never mutates source stream
project .tokmon cannot add a trust root
atomic path swap exposes either E or E+1, never a mixture
old code unload waits for every Beam and path reference
```

随机生成 Photon、path、generation、Act、stop、crash 和 queue pressure 序列验证上述性质。

### 20.3 光流发动机测试

必须覆盖解释文档 6.5 的真实闭环：

```text
user.input("128 * 4")
→ CalculatorLens.view exposes calculate schema
→ deterministic test Rhea emits structured tool call
→ Techor decodes CalculateArgs
→ CalculatorLens.refract performs parser calculation
→ tool.result(512) appended
→ next step observes result
→ Rhea emits answer with no new Act
→ ray.darkened
```

测试 Rhea 是一个签名测试 Lens artifact，通过相同 LightPath 和 Act 管线运行；生产发动机本身没有 hard-coded response。

还要覆盖：多工具调用、unknown tool、wrong schema、old epoch、拒绝、审批、超时、结果未知、振荡、budget exhausted 和 cancel。

### 20.4 Golden ray

Fixture 包含：

- input Photon；
- exact LightPath manifest/hash；
- deterministic Lens artifact versions；
- expected Surface contributions；
- expected Act sequence；
- expected terminal Photon family 和因果边；
- redaction expectations。

时间戳、随机 id 和平台路径通过规范化比较；payload 和语义顺序严格比较。

### 20.5 Crash matrix

注入崩溃点：

- payload write 前/后；
- SQLite transaction 前/中/后；
- Act admitted 后、target 前；
- 现实执行后、result append 前；
- mount epoch append 前/后；
- LightPath atomic swap 前/后；
- worker output 半帧；
- afterglow 中；
- `.tokmon/light-path.yaml` atomic rename、半写、重复 watcher 通知；
- 新 worker ready 前和 IPC endpoint 切换后；
- desktop snapshot 中；
- updater handoff 中。

每个点验证：光子不丢序、不重复现实 Act、状态不伪造、恢复决定可解释、必要时进入 outcome unknown。

### 20.6 ABI 与隔离

- C ABI layout/size/alignment golden；
- 上一 minor SDK artifact 装载；
- malformed CBOR、超大 frame、异常和 crash fuzz；
- allocator ownership；
- worker restart/reconnect；
- native、Node.js、CPython 对同一 Worker Protocol fixture 产生一致 frame 与错误码；
- JavaScript rejected Promise、Python exception、进程异常退出都映射为结构化 `ErrorFrame` 和新 Photon；
- `AbortSignal`、Python cancellation、deadline 和强制 kill 的竞态测试；
- stdout/stderr 洪泛、半行和伪造 protocol 文本不能破坏专用 IPC；
- Node.js/CPython exact runtime 不匹配时拒绝装镜；不同 runtime version 的新旧 worker 可在 handoff 窗口并存；
- TypeScript artifact 不依赖运行时 TypeScript compiler，构建输出是确定性 ESM；
- npm lock/dependency bundle/tree hash 与 Python hash-lock/wheel bundle/environment hash 验证；
- Node native addon、CPython C extension 的 platform/architecture/ABI mismatch 拒绝测试；
- CPython 单 worker GIL 压力与多 worker 水平并行测试；
- WASM fuel/memory/host import；
- signature、quarantine、permission difference；
- dynamic library unload sanitizer test。

### 20.7 Slint UI

- light/dark/high contrast screenshot；
- P0 页面和主要 error state；
- 10k timeline virtualization；
- sustained token/terminal stream；
- cursor gap snapshot；
- desktop crash/reconnect；
- Termon R2 handoff；
- keyboard、screen reader、IME、CJK/emoji/RTL/DPI；
- callback 生命周期与 weak handle；
- UI 线程无阻塞 I/O 检查。

### 20.8 安全测试

- path traversal、symlink/reparse race；
- shell injection、argument confusion；
- secret in Photon/log/UI/crash bundle；
- model prompt data 尝试升级为 instruction；
- approval hash mismatch；
- old epoch Act replay；
- unauthorized host API；
- sandbox escape probes；
- malicious Lens artifact；
- supply-chain signature downgrade。

---

## 21. 旧版迁移

### 21.1 迁移原则

迁移产品行为、schema、fixture、协议经验、C++20 可复用算法、安全经验和 UI 资产；不迁移旧类层级、全局 signal 图、可变单例和 White UI 栈。

旧数据永不直接改写成“新历史”。导入工具读取旧记录，在新库中追加 `migration.imported` Photon，包含来源文件 hash、旧 id、转换版本和错误报告。

### 21.2 旧模块映射

| 旧目录/能力 | 新位置 | 处理 |
| --- | --- | --- |
| `common` | sdk/protocol/base | 只保留平台、hash、Result 等通用 C++20 代码 |
| `axon` signal/executor | Lemon + engine | 改为直线有界光纤，移除全局 signal |
| `arche` runtime | Nyxia | 只迁移验证过的平台封装，不迁移旧抽象模型 |
| `snow` CLI | Snow Lens | 保留命令需求，改为 CliSurface/view/refract |
| `white` UI | Termon Lens | 不迁移渲染栈，只迁移 UX 需求与参考资产 |
| trace/event store | Photon/Chora/Tracket | 转成只追加 schema 和 golden ray |
| workspace/Git | Cove | 加入 canonical path、Act 和结果验证 |
| policy/secret/sandbox | Fallen/Cista/Styx | 统一进入固定 Act 管线 |

### 21.3 Strangler 路线

```text
old tokmon remains runnable
→ build photon/schema/protocol libraries
→ implement Nyxia + Chora + Tracket + Lemon
→ import old transcript read-only
→ implement Snow headless path
→ implement Janus/Rhea/Textus/Techor minimal ray
→ implement safety and Cove
→ build Termon Slint desktop
→ port remaining named lenses one by one
→ differential product tests
→ switch default executable
→ archive old runtime as migration fixture
```

### 21.4 数据导入

1. 对旧库和文件做只读快照；
2. 计算源 hash；
3. 映射到新 Photon schema；
4. 每批事务追加并生成 import report；
5. 无法映射的数据存 immutable artifact reference；
6. Tracket 验证新流；
7. Termon 对比旧会话显像；
8. 用户确认后才切换默认数据目录。

---

## 22. 实现路线图

### Phase 0：Executable optical spec

交付：

- C++20 `PhotonEnvelope/CausalRay/ILens/Act/SurfaceBuilder`；
- CalculatorLens 完整示例；
- 单进程 RayTracingEngine；
- 真实 parser 计算闭环；
- 只追加内存流；
- Lens Contract Suite 初版。

退出：6.5 对应闭环无 mock 地完成真实计算，且无新 Act 自然停机。

### Phase 1：Durable photons

交付：Chora SQLite/blob、Tracket schema/hash、PhotonEmitter、fork、checkpoint、crash injection。

退出：update/delete 被物理拒绝；崩溃恢复通过；导出/回放一致。

### Phase 2：Nyxia optical bench

交付：LightPathSnapshot、LensMount、MountGuard、BeamTicket、OpticalHost、C ABI loader、artifact verification、inspector。

退出：随机装卸与停止测试无后台残留，新光束不进入旧 generation。

### Phase 3：R1/R2 replacement

交付：用户级/项目级 `.tokmon` resolver、YAML watcher、Ignis、dark lane、mount epoch、原子 LightPath 发布、afterglow、Worker Protocol、native worker、Node.js/CPython runtime catalog 与 adapter、TypeScript/Python SDK、离线 dependency materializer、WASM、Lemon bridge、Chora writer handoff。

退出：十九个 Lens 的替换路径都能被 contract test 驱动；C++/Node.js/CPython 等价测试 Lens 通过同一 golden ray；失败不污染 active path。

### Phase 4：Minimal Agent

交付：Rhea、Janus、Textus、Techor、Fallen、Cista、Styx、Cove、Snow，支持对话、模型、计算器、文件读写、审批、终端和自然停机。

退出：完整 Fact → Lens → Act → new Fact golden ray 通过。

### Phase 5：Termon Slint

交付：P0 页面、snapshot/delta、stream batching、ListView、Markdown、terminal、Diff、approval、Lens inspector、R2 handoff。

退出：三平台 UI smoke、截图、流式压力、崩溃重连和无障碍基线通过。

### Phase 6：Advanced lenses

交付：Iris、Clotho、Aya、Enso、Nota、RAG、workflow、sub-run、MCP/LSP、诊断包。

退出：所有二十透镜详细契约和端到端场景通过。

### Phase 7：Migration and release

交付：旧数据 importer、updater、backup/restore、SBOM、签名、安装包、benchmark 和安全审计。

退出：Windows/macOS/Linux 安装、升级、恢复、doctor 和 rollback-as-new-epoch 全通过。

---

## 23. 验收清单

### 纯透镜语义

- 只有 Fact、Photon、CausalRay、Lens、Surface、Act、LightPath、Beam 等透镜原生对象；
- 每个业务 Lens 都实现 `view/refract`；
- Lens 之间没有直接调用或横向消息网；
- Prompt/UI/CLI 可从 Photon + LightPath 重建；
- 5 分钟 CalculatorLens 真实计算并追加结果；
- engine 实现线性 pull fold、动态萃取、真实折射和自然停机。

### 因果光子流

- committed Photon 不能 update/delete/replace/revoke/seq reuse；
- correction、compensation、cancel、fork、merge、recovery 都追加新 Photon；
- append-before-observe；
- payload/hash/parent/schema/epoch 验证；
- checkpoint/索引可删重建，不成为事实源；
- Chora 单 writer 和 crash matrix 通过。

### 动态装卸

- Nyxia 是唯一静态元框架微内核；
- 其余十九 Lens 都有 R1 或 R2 新代码换代路径；
- 用户级 `~/.tokmon/light-path.yaml` 与项目级 `<workspace>/.tokmon/light-path.yaml` 可以声明期望组合；
- YAML 变化先追加 observed Photon，再由 Ignis 提出 reconcile Act；
- candidate 经过 signature、dark lane 和 contract test；
- epoch durable 后才发布 LightPath；
- C++20 原子发布保证并发读者只看到完整 E 或完整 E+1；
- 拔镜后新 view、新 Act 匹配和新 Beam 归零；
- afterglow 有 deadline，guard 活动可枚举、可停止；
- 第三方 native Lens 默认采用 worker 进程 handoff，Node.js/CPython Lens 强制采用 worker endpoint handoff；
- 新旧 Node.js/CPython runtime 与依赖环境可在换代窗口并存，原子发布后不再把新光束路由给旧 worker；
- 历史和现实结果不因拔镜改变。

### Act 与安全

- 所有现实动作走唯一固定管线；
- approval 绑定 act hash、epoch、generation；
- secret 明文不进入 Photon/Surface/log/UI/crash bundle；
- unknown native code、Node native addon 和 CPython C extension 固定 worker/WASM，不得进入 daemon；
- sandbox strength 不静默降级；
- external irreversible 使用幂等、补偿和 outcome unknown 语义。

### C++20、多语言 Lens 与 Slint

- daemon、launcher、CLI、desktop、native worker 与 C++ SDK 使用 ISO C++20，不使用 C++23；
- C++ 宿主/native Lens 的可恢复错误使用 `tl::expected<T, Error>`，正常失败路径不依赖异常；JavaScript/Python SDK 把可预期失败编码为协议 `Result`，未捕获异常只在 worker 边界转成 `ErrorFrame`；
- C++ 进程统一使用 `spdlog`；Node.js/CPython Lens 日志经 `log.write` 交给 host `spdlog`，全部经过脱敏且不作为事实源；
- TypeScript 构建为 Node.js ESM，JavaScript/TypeScript 可以使用锁定、离线物化的 npm 依赖；
- Python Lens 使用 exact CPython 与按 artifact hash 隔离的不可变环境，可以使用锁定、离线物化的 PyPI wheel；
- Node.js/CPython 不嵌入 `tokmond`，语言 runtime、SDK、依赖树与扩展 ABI 均被 lock/hash/SBOM 约束；
- Tokmon 运行配置、信任配置、bootstrap lock 和 Lens manifest 全部使用 YAML/yaml-cpp；
- Slint 只存在于 Termon/desktop，并固定精确版本；
- C++ 句柄、weak handle 和 event-loop 线程规则正确；
- P0 页面闭合输入、计划、审批、执行、Diff、终端、完成和恢复；
- sustained stream、10k trajectory 和大终端输出可交互；
- CJK/emoji/RTL/IME、DPI、键盘和无障碍有测试；
- desktop crash/换代不影响 daemon ray，按 cursor 重建。

### 发布

- artifact signature、lock、SBOM、license、provenance 完整；
- ABI/loader/worker/sandbox/keyring/updater 专项审计；
- protocol/schema 兼容通过；
- database upgrade 先备份且可恢复；
- 三平台 install/update/doctor/smoke 通过。

---

## 24. 关键决策与风险

### 24.1 ADR 清单

```text
0001-a-lens-to-them-all-fact-lens-act.md
0002-causal-photon-stream-is-strictly-append-only.md
0003-committed-photons-have-no-update-delete-replace-or-revoke.md
0004-nyxia-is-the-static-cpp20-primal-prism.md
0005-all-other-nineteen-lenses-are-runtime-replaceable.md
0006-every-lens-has-view-and-refract.md
0007-light-path-is-linear-and-pull-folded.md
0008-no-new-act-means-natural-darkness.md
0009-photon-pattern-matching-is-schema-generated.md
0010-lenses-have-no-normative-private-state.md
0011-native-dynamic-boundary-is-versioned-c-abi.md
0012-unknown-native-lenses-run-in-workers.md
0013-append-before-observe-is-mandatory.md
0014-chora-is-the-only-photon-writer.md
0015-act-has-one-admission-and-refraction-pipeline.md
0016-mount-epoch-is-durable-before-light-path-swap.md
0017-termon-uses-slint-aot-and-process-handoff.md
0018-desktop-is-a-rebuildable-surface.md
0019-cpp20-is-the-language-baseline.md
0020-checkpoints-are-disposable-acceleration.md
0021-script-lenses-use-nodejs-and-cpython-workers.md
0022-language-workers-share-one-lens-worker-protocol.md
0023-runtime-and-dependency-environments-are-exact-and-immutable.md
```

前三条、Nyxia 静态边界、十九 Lens 可换代、`view/refract`、单向光路、只追加 Photon 和唯一 Act 管线属于架构宪法。改变它们等于创建新架构，不是普通实现 ADR。

### 24.2 风险表

| 风险 | 后果 | 控制 |
| --- | --- | --- |
| Lens 偷藏规范状态 | 换代/恢复不一致 | stateless review、replay test、worker kill test |
| `view` 偷做 I/O | 折叠卡顿、结果不确定 | restricted host、thread check、contract test |
| 直线光路演变成隐藏横向调用 | O(N²) 耦合 | SDK 无 Lens lookup API、link audit |
| ActPattern 冲突 | 错目标执行 | mount-time uniqueness validation |
| Photon 无限增长 | 磁盘与读取压力 | immutable segment、blob、checkpoint、retention access policy |
| afterglow 卡住 | 代码无法卸载 | deadline、stop、worker termination、diagnostic |
| in-process native 崩溃 | daemon 崩溃 | 只允许高信任签名代码，其他 worker/WASM |
| npm/PyPI 依赖供应链污染 | 任意代码执行、数据泄漏 | exact lock、offline bundle、hash、SBOM、签名、quarantine、漏洞/许可证扫描 |
| Node.js/CPython runtime 漂移 | 同一 Lens 行为或 ABI 不一致 | exact runtime hash、平台专用 artifact、dark-lane contract、按 epoch 换代 |
| script worker 资源滥用 | CPU/内存/进程/输出耗尽 | Styx/OS quota、deadline、bounded IPC、kill process tree |
| native addon/C extension ABI 错配 | worker crash 或内存破坏 | 平台/architecture/ABI 精确锁定、worker 隔离、装镜前加载测试 |
| writer handoff 双写 | seq/hash 破坏 | 单 writer token、commit barrier、crash test |
| 模型重复调用 | ray 振荡 | natural darkness、dedupe、budget、approval |
| UI token flood | 主线程卡顿 | batch、ListView、ring、snapshot |
| secret 进入 Photon | 永久泄漏 | Cista 多出口扫描、commit/send gate |
| 外部结果不确定 | 重复现实动作 | idempotency、provider query、outcome unknown |
| Slint 升级回归 | UI/打包失败 | exact tag、ADR、三平台截图/性能/license gate |

### 24.3 设计变更规则

任何跨 Lens 的新能力必须回答：

1. 它观察哪些 PhotonPattern？
2. 它向哪个 SurfaceChannel 做 `view` contribution？
3. 它接受哪个 ActPattern？
4. `refract` 产生哪些新 Photon？
5. 现实动作经过哪条固定 Act 管线？
6. 拔镜后如何保证后续零新贡献？
7. 如何回放、崩溃恢复和测试？
8. 如何在 C++20、C ABI、Node.js/CPython Worker Protocol 或 WASM 中实现？

如果答案需要可变全局对象、横向调用、历史改写或第二事实源，该设计直接拒绝。

---

## 25. 参考

项目内规范资料：

- [`tokmon-lens-architecture-explained.zh.md`](tokmon-lens-architecture-explained.zh.md)
- [架构论文中文版](everything-is-a-lens-paper.zh.md)
- [架构论文英文版](everything-is-a-lens-paper.en.md)
- [`advise.md`](advise.md)
- [旧 C++ 总体设计](../../tokmon/docs/DESIGN.md)
- [旧 UI 架构](../../tokmon/docs/WHITE_UI_ARCHITECTURE.md)
- [旧 UI 参考资产](../../tokmon/assets/reference/)

历史背景论文只用于理解架构演进动机，不参与本文的术语、接口、对象或执行模型。

非规范历史背景：

- [《A Programming Paradigm for Spatiotemporal Composability》](../../A%20Programming%20Paradigm%20for%20Spatiotemporal%20Composability.pdf)——仅用于追溯架构演进，不从中导入概念。

Slint 官方资料：

- [Slint GitHub repository](https://github.com/slint-ui/slint)
- [Slint C++ API](https://docs.slint.dev/latest/docs/cpp/)
- [Slint C++ CMake integration](https://github.com/slint-ui/slint/blob/master/api/cpp/README.md)
- [Slint ListView](https://docs.slint.dev/latest/docs/slint/reference/std-widgets/views/listview/)
- [Slint type mappings](https://docs.slint.dev/latest/docs/cpp/types/)
- [Slint releases](https://github.com/slint-ui/slint/releases)
- [Slint license](https://github.com/slint-ui/slint/blob/master/LICENSE.md)

最终系统只有一个闭环：

> **Fact 是不可更改的来路，Lens 以 view 显出当前视界，以 refract 承接受控 Act；现实结果只能化成新的 Photon，沿同一束因果光继续向前。**

Nyxia 守住光学定律，RayTracingEngine 推进每一拍，十九个动态透镜决定光如何被看见与折射，Chora 和 Tracket 守住不可改写的光痕，Termon 用 Slint 把当下的视界显像给人。镜片可以更换，视界可以改变，历史永不重写。
