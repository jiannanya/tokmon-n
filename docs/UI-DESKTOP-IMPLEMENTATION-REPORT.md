# Tokmon Desktop UI、进程生命周期与端到端实现验收报告

> 2026-08-23 最新增补：真实 OpenCode、项目/分组/会话持久化、会话 Ray 恢复、原生 UI 自动化及 `84/84` 回归结果见 [OPENCODE-DESKTOP-ACCEPTANCE-REPORT.md](OPENCODE-DESKTOP-ACCEPTANCE-REPORT.md)。本文中标注为早期 mock、演示文件名或 `83/83` 的段落仅保留实现演进记录，不代表当前状态。

## 斜杠命令选择器补充（2026-08-23）

输入框以 `/` 开头且尚未进入参数区时，Desktop 从共享命令目录实时筛选最多 8 条结果。浮层展示规范用法、中文说明和类别，支持悬停高亮与点击回填；输入空格、发送或选择后自动收起。所有命令由 `UiSnowController` 发送给 `tokmond`，结果再投影回对话、轨迹、模型/权限/强度状态、设置弹窗或系统剪贴板。

> 后续更新：真实多平台模型配置、系统凭据库与 Rhea 联网链路已完成，见 [MODEL-PROVIDER-IMPLEMENTATION-REPORT.md](MODEL-PROVIDER-IMPLEMENTATION-REPORT.md)。第 6 节的 mock 闭环仅记录早期验收快照；当前生命周期和测试结论已更新。

> 产品题记：**A Lens to Them All**  
> 实现语言：C++20 + Slint C++  
> 配置格式：YAML  
> 错误通道：`tl::expected<T, tokmon::Error>`  
> 日志：spdlog  
> 验收日期：2026-08-23

## 1. 验收结论

本轮需求已经形成可运行实现，而不是静态设计稿：

- `tokmon-desktop` 是原生无边框 Slint 工作台，完成主界面和八个设置页；
- 搜索、树折叠、新建项目/会话、标题编辑、左右面板、页签、步骤、输入发送、选择菜单、文件菜单、问题面板和设置弹窗均有可操作状态；
- 新建项目/会话弹窗可输入或通过原生目录对话框选择工作空间；项目拥有目录，会话默认继承并可覆盖，节点切换会真实切换对应 tokmond、Snow endpoint、Ray 和文件根目录；
- `tokmon` 与 `tokmon-desktop` 均可自行连接或后台拉起同目录 `tokmond`，不依赖 `tokmon-launcher`；
- Desktop 关闭后释放带心跳的客户端租约；最后一个客户端和活动工作离开后，对应 workspace daemon 自动优雅停机；
- CLI 与 Desktop 的输入都进入真实 Nyxia 光路。模型 provider 暂用允许的 deterministic mock，但模型产生的 `model.tool-call` 会路由到真实 Calculator Lens，实际执行后再由模型生成最终回答；
- Photon 事实流仍只追加。UI、设置和客户端生命周期没有获得修改、删除或撤销历史 Photon 的旁路；
- Windows Slint 构建成功，UI-off 构建成功，当前自动化测试 `84/84` 通过；
- 主界面及八个设置页均完成 2160×1350 物理像素实机截图，八个设置页启动、渲染、关闭的 stderr 均为 0 字节。

## 2. Figma 对齐基线

实现逐节点读取并对齐以下设计画面：

| 画面 | 新版 Figma node |
| --- | --- |
| 主工作台 | `1:3` |
| 轨迹 | `1:907` |
| 通用设置 | `1:2043` |
| 智能体与模型 | `2:3360` |
| 权限与安全 | `2:4447` |
| 工作区 | `2:5526` |
| 通知 | `2:6601` |
| 外观 | `2:7673` |
| 快捷键 | `2:8765` |
| 账户 | `2:9869` |

设置弹窗按设计采用 1120×720 逻辑像素：顶部 56、底部 56、左侧导航 220、右侧概览 230，中间为内容区。主窗口基准是 1440×900 逻辑像素，支持缩放、最小尺寸和左右面板拖拽。

视觉系统采用新版设计稿的暖白表面、stone 灰文字、低饱和棕橙强调色、1px 分隔线、16px 卡片圆角与低层级阴影。主要 token 为 `#f9f9f8`、`#fbfbf9`、`#fdfbf7`、`#f7efe5`、`#8b5229`、`#ebdcd0`；开关使用 `#c86a28`。窗口使用 `no-frame: true`，并在 Windows 显示后移除原生 caption style；顶部 30px 是唯一的自绘标题栏，包含拖动、最小化、最大化/还原和关闭。

## 3. 主工作台实现

### 3.1 布局

工作台由四个稳定区域组成：

1. 自绘无边框标题栏；
2. 左侧项目/会话树；
3. 中央对话与因果轨迹工作区；
4. 右侧代码审阅与文件预览面板。

左、右面板都可隐藏、恢复和拖拽调整宽度。中央区域根据两侧可见状态动态计算最小宽度，不依赖固定屏幕截图。

### 3.2 交互验收矩阵

| 需求 | 实现行为 | 状态/后端效果 | 结果 |
| --- | --- | --- | --- |
| 搜索 | 双向绑定搜索文本，输入时刷新导航模型 | 过滤自身命中项，并保留含命中后代的父节点 | PASS |
| 树折叠 | 点击可展开节点修改 `expanded` | 无搜索时按祖先状态隐藏后代 | PASS |
| 新建项目 | 导航标题栏 `+` 打开名称/工作空间弹窗 | 写入规范化绝对路径并切换隔离 tokmond | PASS |
| 新建会话 | 顶部按钮打开会话/工作空间弹窗 | 默认继承父项目，可覆盖；创建后清空活动 ray，不删除历史 Photon | PASS |
| 切换项目/会话 | 点击树节点解析其有效工作空间 | 原子交接 daemon 租约；恢复对应 Ray 或进入空会话 | PASS |
| 标题编辑 | 铅笔按钮在 Text/LineEdit 间切换 | Enter 接受并保留新标题 | PASS |
| 左右面板开合 | 标题栏与面板关闭按钮双向控制 | 中央区重新布局 | PASS |
| 面板调整 | 两条 6px resize handle | 限制在安全最小/最大宽度内 | PASS |
| 对话/轨迹 | 中央页签切换 | 对话显示用户、最终答复和当前回合真实工作流；轨迹显示统计、三泳道时间条、Photon 事件表与 Request 摘要 | PASS |
| 代码/文件 | 右侧页签与文件菜单切换 | Python 代码模型和 `output.srt` 预览状态分离 | PASS |
| 步骤收起 | 任务卡头部控制展开状态 | 折叠为 42px，展开显示可滚动步骤 | PASS |
| 输入发送 | composer 双向绑定，发送后清空 | 携带模型、权限和强度，经 Snow 提交 daemon | PASS |
| 权限菜单 | 完全访问/受限访问/只读模式 | 选择进入 `user.input` context | PASS |
| 模型菜单 | 本地、语音与 cloud-auto 选项 | 选择进入 `user.input` 和 Janus `model.call` | PASS |
| 强度菜单 | 最高/中等/较低 | 选择进入同一不可变输入 Photon | PASS |
| 附件菜单 | 文件/文件夹入口 | 形成明确待提交 composer 状态 | PASS |
| 文件菜单 | `transcribe.py` / `output.srt` | 切换代码与文件预览 | PASS |
| 代码菜单 | 复制路径、在资源管理器显示、关闭面板 | 菜单状态与面板状态完整 | PASS |
| 问题面板 | 状态栏“2 个问题”开关 | 显示/隐藏结构化问题卡片 | PASS |
| 设置弹窗 | 左下角设置入口 | 模态遮罩、八页导航、保存/取消/恢复默认 | PASS |

## 4. 八个设置页

设置不是独立静态图片，而是同一个可切换、可保存的状态模型。

| 页面 | 可操作字段 |
| --- | --- |
| 通用 | 应用语言、启动位置、自动保存、更新通道展示 |
| 智能体与模型 | 默认智能体展示、provider、主模型、推理强度 |
| 权限与安全 | 文件访问展示、命令审批、网络访问、高风险二次确认 |
| 工作区 | 默认工作区、索引模式、自动同步、Git 集成 |
| 通知 | 总开关、桌面通知、消息提醒、免打扰 |
| 外观 | 浅/深主题选择、六个强调色、密度、字体比例 |
| 快捷键 | 四个绑定项和重新录制等待状态 |
| 账户 | 头像、昵称、邮箱、方案、云同步 |

“保存更改”把全部 UI 字段作为 Snow intent 发给 `tokmond`。daemon 只更新当前项目 `.tokmon/config.yaml` 的 `ui:` 节点，写入同目录 `.new` 后使用 Windows `MoveFileExW(...REPLACE_EXISTING | WRITE_THROUGH)` 或 POSIX rename 原子发布。下次 Desktop 连接时通过 `settings.get` 读取。其他客户端只会看到下一次读取结果；历史 Photon 不被设置写入修改。

## 5. Desktop 与 daemon 启停流程

### 5.1 统一启动

`tokmon` 和 `tokmon-desktop` 使用相同生命周期库：

```text
解析 --workspace
  → resolve ~/.tokmon 与 <workspace>/.tokmon
  → 由 canonical workspace 计算独立 Snow endpoint
  → 短超时 ping
      ├─ 已存在：直接连接
      └─ 不存在：定位当前程序同目录 tokmond
           → 后台无窗口启动 tokmond --workspace ... --endpoint ...
           → 轮询 ready（上限 8 秒）
           → 连接
```

Windows 使用 `CreateProcessW` 和 `CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP`；POSIX 使用 `fork`、`setsid`、`execl`。workspace 与 endpoint 均作为独立 argv 传递，不经 shell 拼接执行。

### 5.2 单实例与隔离

- endpoint 由规范化 workspace hash 派生，不同项目不会误连同一 daemon；
- Windows 使用 endpoint hash 派生的 named mutex；
- POSIX 使用 endpoint 邻接 lock file 和 `flock(LOCK_EX | LOCK_NB)`；
- Snow request ID 使用每进程随机 64 位前缀与原子序列，避免不同客户端重用小整数导致 daemon 幂等缓存串响应。

### 5.3 关闭语义

- 关闭 Desktop：释放 Desktop 租约；无其他客户端和活动工作时，250 ms 后停止对应 daemon；
- Desktop 崩溃：6 s 心跳租约过期后执行同样的安全回收；
- CLI `chat`/`stdio`：会话期间续租，退出后 250 ms 空闲停止；
- CLI 一次性命令：保留 15 s 命令串复用窗口，之后空闲停止；
- `tokmon daemon status`：只做健康探测；
- `tokmon daemon start`：显式启动并 pin，直到 `tokmon daemon stop`；
- `tokmon daemon stop`：发送 `daemon.shutdown`，在活动工作完成后优雅结束；
- `tokmon-launcher`：仅启动 Desktop 的兼容快捷方式，不创建也不拥有 daemon。

多客户端不会互相误杀：只有最后一个租约离开且 Nyxia 没有活动工作时才能自动停止。详细协议和验收见 [DAEMON-LIFECYCLE.md](DAEMON-LIFECYCLE.md)。

## 6. 从对话到真实 Act 的闭环

当前 mock 仅替代外部大模型网络调用，不替代 Agent、路由、权限或工具执行：

```text
CLI / Desktop 文本
  → Snow chat intent
  → append user.input（含 model/access_mode/effort）
  → Janus 观察 Fact 并提出 model.call Act
  → Rhea local-deterministic 产生 reasoning 与 model.tool-call
  → Techor 按 schema 路由 tool.calculate
  → Fallen 公共 admission
  → Calculator Lens 在 Beam 中真实计算
  → append tool.result
  → Janus 提出后续 model.call
  → Rhea 生成 assistant.message
  → ray.darkened
  → Termon/Snow 投影给 Desktop/CLI
```

验收输入：

```text
请帮我计算 6 * 7，然后告诉我结果
```

验收事实包含：

- `model.reasoning-chunk`：识别算术意图；
- `model.tool-call`：`{"expression":"6 * 7"}`；
- `tool.result`：真实结果 42；
- `assistant.message`：`计算完成，结果是 42`；
- `ray.darkened`：没有新 Act 后自然结束。

## 7. UI 事实边界与健壮性

Termon/Slint 只保存可丢弃的 projection cache。发送消息、保存设置、reconcile 等操作均通过 Snow 发送 intent，Desktop 不直接持有 `TokmonRuntime`、Photon append gate、凭据或透镜动态库。

审计截图阶段发现旧数据库中的历史 Photon 含早期 Windows argv 编码问题留下的非法 UTF-8。Slint 的 Rust runtime 会拒绝该字符串。Desktop 现在在所有外部文本进入 Slint 前进行严格 UTF-8 扫描：合法 code point 原样保留，非法、过长、surrogate 或越界序列替换为 U+FFFD。修复后同一历史数据库启动、snapshot、渲染、关闭均为 exit 0，stderr 为 0。

Windows CLI 同时改用 `CommandLineToArgvW` 和明确 UTF-8 转换，因此新的中文输入不会再写入 mojibake 或非法字节。

### 7.1 两种工作流投影

Desktop 不再把“模型返回”简化成单一结果字符串：

```text
当前回合对话投影
  = 从最后一个 user.input/user.message 起
    筛选 model.* / act.* / tool.result / assistant.message / workflow.*

完整轨迹投影
  = 当前 Ray Surface 中的全部已提交 Photon
```

因此用户可以在对话页快速看到“已发送模型请求 → 模型组合透镜能力 → 透镜行动 → 工具结果 → 最终答复”，也可以进入轨迹页查看完整不可变证据。折叠工作流只改变本地展示状态，不会删除或修改任何 Photon。

## 8. 编译、测试与实机证据

### 8.1 构建

```powershell
cmake --build build/windows-msvc-ui-debug --target tokmond tokmon tokmon-desktop tokmon-tests -j 4
cmake --preset portable-debug
cmake --build build/portable-debug --target tokmond tokmon -j 4
```

结果：全部成功。Windows UI 构建生成 `tokmond.exe`、`tokmon.exe`、`tokmon-desktop.exe` 和 Slint runtime；UI-off preset 也成功。项目保持 C++20，没有引入 C++23。编译输出中的提示来自第三方 yaml-cpp/spdlog/fmt 或既有兼容代码，没有本次目标的编译/链接错误。

### 8.2 自动化测试

```powershell
ctest --test-dir build/windows-msvc-ui-debug --output-on-failure -C Debug
```

当前回归结果：`84/84` 通过，`0` 失败；包含客户端租约 attach/heartbeat/detach/pin 合约测试。

本轮新增或强化的自动测试包括：

- 对话式中文算术意图产生真实工具调用；
- UI 选择的 model/access/effort 保留到不可变 `user.input` Photon；
- 工作区 endpoint 隔离与 daemon 健康探测；
- Snow CLI stdio 使用同一 workspace endpoint，继续保证 stream 在 final 前且 ordered close 最后；
- 既有二十透镜动态库、每镜折射、热替换、Node.js、CPython、MCP、LSP、PTY、Git、存储和 append-only 测试全部保持通过。

### 8.3 UI 截图

截图目录：`build/ui-qa-current/`。

| 文件 | 内容 |
| --- | --- |
| `main.png` | 当前无边框主工作台、真实 Photon 轨迹与代码投影 |
| `settings-general.png` | 通用 |
| `settings-agents-models.png` | 智能体与模型 |
| `settings-permissions-security.png` | 权限与安全 |
| `settings-workspace.png` | 工作区 |
| `settings-notifications.png` | 通知 |
| `settings-appearance.png` | 外观 |
| `settings-shortcuts.png` | 快捷键 |
| `settings-account.png` | 账户 |

九张图均为 2160×1350 物理像素；设置页由 `--open-settings --settings-page 0..7` 进入 QA 状态，仅用于可重复截图，不改变普通启动行为。

## 9. 最终审计

| 审计项 | 结论 |
| --- | --- |
| C++ 标准是否保持 C++20 | PASS |
| Desktop 是否无边框且有完整窗口控制 | PASS |
| Figma 主界面和八设置页是否实现 | PASS |
| 指定前端交互是否具有可操作状态 | PASS |
| CLI/Desktop 是否无需 launcher 即可使用 | PASS |
| daemon 是否按 workspace 隔离且单实例 | PASS |
| Desktop 是否自动回收对应 daemon 且不误杀其他客户端/活动工作 | PASS |
| daemon 是否可显式优雅停止 | PASS |
| mock 模型后是否存在真实 Agent/Act/工具执行 | PASS |
| UI 是否只做投影而不旁路修改 Photon | PASS |
| 配置是否使用项目级 `.tokmon/config.yaml` YAML | PASS |
| 错误与日志库是否保持 `tl::expected` / spdlog | PASS |
| 自动化测试是否全绿 | PASS，84/84 |
| 是否需要用户补 Rust 工具链 | 不需要 |

## 10. 关键实现入口

- Slint 视觉与交互：`apps/tokmon-desktop/ui/tokmon.slint`
- Desktop Snow controller 与 UTF-8 边界：`apps/tokmon-desktop/main.cpp`
- 共享 daemon lifecycle：`protocol/daemon_lifecycle.cpp`
- lifecycle public API：`sdk/cpp/include/tokmon/daemon_lifecycle.hpp`
- Snow request ID 与协议：`protocol/snow_protocol.cpp`
- Snow transport：`protocol/snow_transport.cpp`
- daemon、单实例、设置和 shutdown：`apps/tokmond/main.cpp`
- CLI 自动连接与 daemon 命令：`apps/tokmon-cli/main.cpp`
- 可选 launcher：`apps/tokmon-launcher/main.cpp`
- mock model 工具选择：`lenses/rhea/rhea_lens.cpp`
- Agent 多拍推进：`lenses/janus/janus_lens.cpp`
- 测试：`tests/unit/core_tests.cpp`、`tests/unit/builtin_lens_tests.cpp`
