# Tokmon 新版实现报告

> 架构题记：**A Lens to Them All**  
> 实现语言：C++20  
> 报告日期：2026-08-22  
> 设计基线：[DESIGN.md](./DESIGN.md)  
> 内置透镜详设：[BUILTIN-LENSES-DESIGN.md](./BUILTIN-LENSES-DESIGN.md)

## 1. 交付结论

新版 Tokmon 已在 `tokmon-n/` 下形成可编译、可运行、可测试的 C++20 实现。Nyxia 是不可替换的微内核；业务能力全部通过 Fact/Photon → Lens → Surface/Act → new Fact 闭环表达。十九个正式内置透镜与 Calculator 参考透镜均已从目录占位替换为独立 C++ 类型、独立 manifest、独立契约测试和独立动态库产物。

本次收口还完成了：

- 严格 YAML manifest 读取与未知字段拒绝；
- 基于 `tl::expected` 的统一错误边界；
- 基于 `spdlog` 的进程日志与敏感内容清洗；
- SQLite Photon 全局 SHA-256 哈希链和物理 UPDATE/DELETE trigger 禁止；
- C ABI 动态透镜装载；
- Node.js 与 CPython 外部进程透镜、canonical CBOR Worker 协议、取消和进程树终止；
- LightPath dark lane、完整图验证、durable epoch commit、原子发布和旧 generation afterglow；
- Windows named pipe / Unix domain socket Snow 本地协议；
- Slint 1.17.1 桌面 UI 与 Figma 视觉基线；
- Debug 测试构建、完整 CTest 回归和带 Slint 的 MSVC 构建。

## 2. 实现结构

```text
tokmon-n/
├─ apps/
│  ├─ tokmond/                 # 唯一 Photon writer 与 Snow server
│  ├─ tokmon-cli/              # 命令行 Snow client
│  ├─ tokmon-desktop/          # Slint Termon 桌面显像
│  ├─ tokmon-launcher/         # 桌面进程启动/交接
│  └─ tokmon-lens-worker/      # Node.js/CPython 受管进程 supervisor
├─ nyxia/
│  ├─ base/                    # CBOR、ID、SHA-256、Error、spdlog
│  ├─ config/                  # 用户级/项目级 .tokmon YAML 合并
│  ├─ engine/                  # RayTracingEngine 与 Act admission
│  ├─ light_path/              # immutable epoch snapshot 与原子发布
│  ├─ loader/                  # YAML manifest 与稳定 C ABI
│  ├─ runtime/                 # reconcile、dark lane、换代、afterglow
│  ├─ storage/                 # append-only PhotonStore
│  └─ worker/                  # Worker protocol 与 WorkerLensProxy
├─ lenses/                     # 十九个正式内置透镜 + Calculator
├─ protocol/                   # Snow framing 与 OS transport
├─ sdk/
│  ├─ c/                       # 稳定 C ABI
│  ├─ cpp/                     # C++20 Lens SDK
│  ├─ typescript/              # Node.js ESM adapter/SDK/示例
│  └─ python/                  # CPython adapter/SDK/示例
├─ config/                     # 默认 config.yaml/light-path.yaml
└─ tests/                      # 核心、跨边界、逐镜契约测试
```

## 3. 二十个透镜的实现状态

十九个正式顺序为：Ignis、Lemon、Iris、Rhea、Janus、Clotho、Aya、Textus、Enso、Techor、Styx、Fallen、Cista、Chora、Tracket、Nota、Cove、Snow、Termon。Calculator 是独立的开发者契约参考实现。

每个 `lenses/<name>/` 当前都有：

- `<name>_lens.hpp`：独立 final 类型与公开契约；
- `<name>_lens.cpp`：该透镜自己的显像、校验、折射和结果 Photon；
- `lens.yaml`：ID、版本、ABI、runtime、trust、observes、channels、refracts、permissions；
- `tests/<name>_contract.cpp`：manifest 等价、确定性、边界和停止语义。

业务实现不位于 registry 或通用基类中。`lens_base` 只提供 manifest 保存、stop、pattern 匹配、稳定 Act 提案和 Beam emit；`builtin_registry` 只包含 ID 到真实构造函数的映射。逐镜算法、参数和结果 Photon 的完整矩阵见内置透镜详设第 25 节。

其中已经触及现实系统并有专项测试的实现包括：

- Styx：使用 argv 启动真实子进程，不经过 shell；Windows Job Object/POSIX process group 负责树级终止，stdout/stderr 有界；
- Cove：根内 canonical path、precondition hash、真实文件 read/write/move/delete、写后读取校验、无 shell Git；
- Chora：真实 SHA-256 内容寻址 blob、export、checkpoint 和 archive；
- Clotho：真实 DAG 解析、缺失依赖和环检测、稳定 ready-node 选择；
- Rhea/Janus/Techor/Calculator：完成三拍 `Fact → model.call → tool.calculate → tool.result → final message` golden ray；
- Snow：真实本地 IPC framing、request id、cursor 和 canonical CBOR；
- Cista：credential、Bearer、assignment 与 URL query 清洗。

## 4. 因果光子流与不可改写性

`PhotonStore` 的每次提交都分配新 sequence，填充 previous hash，使用 canonical CBOR payload 计算 SHA-256，并在 SQLite FULL synchronous 事务中追加。数据库建立 `photons_no_update` 与 `photons_no_delete` trigger，任何修改或删除 committed Photon 的 SQL 都会失败。

取消、失败、补偿、恢复和换镜没有撤销入口：

- Act 成功追加 `act.completed` 和目标结果 Photon；
- Act 失败追加 `act.failed`；
- 取消追加 cancel/terminal Photon；
- 恢复旧 Lens artifact 发布更高 epoch；
- replay/fork 产生新 ray，不改变来源 ray。

自动测试同时验证哈希链、previous hash 连续性以及 UPDATE/DELETE 的物理拒绝。

## 5. 运行时组合与热插拔

`TokmonRuntime::reconcile()` 重新读取用户级 `~/.tokmon/light-path.yaml` 与项目级 `<workspace>/.tokmon/light-path.yaml`，建立完整候选图。每个候选依次完成 artifact hash、manifest/ABI/ID/runtime 校验、进程握手（若需要）和空窗口 dark-lane `view`。完整图还会检查重复 Lens ID 与精确 ActPattern 冲突。

只有所有候选通过后，Nyxia 才先追加 `mount.epoch-committed`，再用一次 atomic shared pointer store 发布 E+1。所有读者只能看到完整 E 或完整 E+1，不存在半张光路。旧 generation 随后停止接收新 Beam，取消并等待在途 Beam，最后释放动态库或整个 Worker 进程树。

换代可观察过程为：

```text
config.light-path-observed
→ lens.candidate-verified | lens.candidate-rejected
→ mount.epoch-committed
→ lens.afterglow-started
→ lens.afterglow-completed
```

这些记录全部追加；恢复旧 generation 也必须产生新的 epoch。

## 6. C++、Node.js 与 CPython 承载

三种承载最终都适配为同一个 C++ `ILens`：

| 承载 | 装载方式 | 换代单位 | 隔离 |
| --- | --- | --- | --- |
| C++ 内置 | 独立具体类型 | generation | Nyxia 进程内 |
| C++ 动态库 | `tokmon_lens_entry_v1` 稳定 C ABI | DLL/shared object generation | 可选进程内 |
| JS/编译后 TS | Node.js ESM adapter | 独立 Worker generation | 强制进程边界 |
| Python | CPython adapter | 独立 Worker generation | 强制进程边界 |

Node.js 与 CPython 不嵌入 `tokmond`。`WorkerLensProxy` 启动 `tokmon-lens-worker`，后者再启动精确解释器与语言 adapter；双方使用 4-byte 大端长度 + canonical CBOR frame。`worker.hello/ready` 校验协议、Lens ID 和 runtime；`view/refract` 返回的 channel、Act proposal 和 Photon emit 还会在 C++ host 侧按 manifest 权限复核。

脚本 artifact 的 `lens.yaml` 必须声明 `runtime.kind/version/entry`。入口经过根内校验，不允许 `..` 逃逸。Node.js 与 CPython 仍可使用各自的 npm/PyPI 生态，但依赖应在构建时锁定并与 artifact 一起固化；运行时不会执行在线 install。

运行时文件位置：

```text
~/.tokmon/runtimes/node/node.exe                 # Windows
~/.tokmon/runtimes/cpython/python.exe            # Windows
~/.tokmon/runtimes/node/bin/node                 # Unix
~/.tokmon/runtimes/cpython/bin/python3           # Unix
```

开发示例位于 `sdk/typescript/examples/adder.mjs` 与 `sdk/python/examples/adder.py`，两者各有可由严格 loader 读取的 `lens.yaml`。

## 7. 配置、错误与日志

- 所有配置采用 YAML；用户级和项目级目录均命名为 `.tokmon`；
- 项目级配置覆盖用户级同 ID Lens，artifact 相对路径以声明它的 `light-path.yaml` 所在目录解析；
- 未知 YAML 字段、未知 runtime、缺少 manifest 字段或错误 ABI 会返回 `schema_mismatch`/`abi_mismatch`，不会发布新路径；
- 可预期错误统一为 `tl::expected<T, Error>`；跨 C ABI/Worker 边界的异常在最外层变为结构化 Error；
- C++ 日志统一使用 spdlog；Worker 日志经 host Beam 回传后进入同一出口，并经过清洗。

## 8. Slint UI

Termon 使用 Slint 1.17.1 C++ SDK。UI 实现保留 Figma 的三栏信息架构、暗色视觉、会话导航、中心工作区、轨迹/审批/代码/终端信息区和底部输入区。图标来自设计资源并以 SVG 资产加载，中文界面已做截图验证。

桌面进程不是事实源：Slint property 与 model 只保存 projection cache；用户操作经 Snow 转为 intent，提交与因果状态仍由 `tokmond` 掌握。

本机使用项目内预编译 Slint C++ SDK，因此本次构建不依赖 Rust 工具链。

## 9. 编译与验证结果

### 9.1 无 UI 的完整测试构建

```powershell
cmake --build build/verify-tests --parallel
ctest --test-dir build/verify-tests --output-on-failure
```

结果：`41/41` 通过，`0` 失败。测试覆盖：

- canonical CBOR；
- append-only Photon store 与哈希链；
- LightPath 原子 epoch；
- C ABI dark lane 与二十个 DLL 身份；
- 十九个正式透镜与 Calculator 契约；
- 每个内置透镜至少一个声明折射场景；
- Calculator 三拍完整因果 ray；
- Styx/Cove/Chora/Clotho 现实行为；
- Node.js/CPython SDK；
- Node.js/CPython `WorkerLensProxy` 真实进程端到端；
- reconcile lifecycle Photon；
- C ABI Calculator generation 经 dark lane 从动态库换为内置实现，并以更高 epoch 原子发布。

### 9.2 带 UI 的 MSVC 构建

```powershell
cmake --build build/windows-msvc-ui-debug --parallel
```

结果：成功生成 `tokmond.exe`、`tokmon.exe`、`tokmon-launcher.exe`、`tokmon-lens-worker.exe`、`tokmon-desktop.exe` 和二十个 `tokmon-lens-<name>.dll`。编译器输出中只有第三方头文件 shadow/deprecation 与 Windows `getenv` 安全提示，没有编译或链接错误。

安装冒烟使用 `cmake --install build/windows-msvc-ui-debug --prefix build/install-smoke` 成功；安装树包含五个应用程序、二十个 Lens DLL、`slint_cpp.dll`、40 个 Figma SVG、默认 YAML 以及 Node.js/CPython adapter。

### 9.3 已验证的外部运行时

- Node.js `v25.8.1`；
- CPython `3.10.7`；
- Slint C++ `1.17.1`。

## 10. 交付文件索引

- 总设计：`docs/DESIGN.md`
- 二十镜详细设计与实现矩阵：`docs/BUILTIN-LENSES-DESIGN.md`
- 本报告：`docs/IMPLEMENTATION-REPORT.md`
- 微内核运行时换代：`nyxia/runtime/runtime.cpp`
- 严格 manifest loader：`nyxia/loader/manifest_io.cpp`
- Worker proxy：`nyxia/worker/worker_lens_proxy.cpp`
- C ABI loader/entry：`nyxia/loader/c_abi_loader.cpp`、`lenses/common/named_lens_entry.cpp`
- 二十镜源码：`lenses/*/*_lens.cpp`
- Slint UI：`apps/tokmon-desktop/ui/tokmon.slint`
- 测试：`tests/unit/` 与 `lenses/*/tests/`

## 11. 工具链结论

当前 Windows 构建已经完整成功，不需要手动补齐 Rust。继续开发所需的必备工具是 CMake 3.25+、Ninja、C++20 编译器；只有在不再使用项目内 Slint C++ SDK、转为从 Slint 源码自行生成工具时，才需要 Rust/Cargo。
