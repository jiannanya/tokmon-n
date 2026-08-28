# Tokmon OpenCode、Desktop 与工作区端到端实现验收报告

> 验收日期：2026-08-24
>
> 验收工作区：`E:\cc\AI\tokmon\tokmon_work_test\test2`
> 实现：C++20、Slint C++、YAML、`tl::expected`、chLog
> 语义边界：Fact → Lens → Act；Photon 只能追加；产品题记为 **A Lens to Them All**

## 1. 结论

本轮已把真实 OpenCode 配置、CLI/Desktop 对话、项目/分组/会话管理和原生 UI 交互连成同一条可运行光路：

- 项目级 `.tokmon/config.yaml` 使用 `OPENCODE_API_KEY` 引导系统凭据，YAML、Photon 和日志不保存 Key 明文；
- 默认平台为 `opencode`，协议为 `openai-compatible`，模型为 `x-preview-f-free`；
- CLI 与 Desktop 均完成真实联网请求；Desktop 鼠标验收现场命中“空内容 → 5 秒重试 → HTTP 503 → 10 秒重试 → 成功”，错误与恢复均如实追加；
- Desktop 的分组、项目、会话、标题、选择和折叠状态原子保存到项目 YAML；
- 项目节点绑定真实文件系统工作空间；会话默认继承所在项目，也可保存独立覆盖目录。选择节点会同步切换 Snow endpoint、tokmond、模型配置、Ray 与文件工具根目录；
- 会话节点现在绑定真实 Ray。切到新会话后再切回旧会话，会通过 `surface(ray)` 恢复用户输入、回答、失败信息和完整 Photon 轨迹；
- 搜索、斜杠命令选择器、原生文件对话框、面板、页签、设置、菜单和无边框窗口控制均通过真实鼠标/键盘操作；
- 点击 Desktop 自绘关闭按钮后 Desktop 退出，对应工作区 tokmond 在租约释放后自动退出；
- Desktop 跨工作空间切换时只保留当前活动工作空间租约；关闭后导航工作空间与活动工作空间的 daemon 均自动退出；
- Windows Slint Debug 构建成功，CTest `84/84` 通过。

### 1.1 新主题、对话工作流与轨迹页增补

本轮继续以新版 `tokmon-design-new` 为唯一视觉基线，完成主工作台、轨迹页及八个设置页的主题迁移。新视觉不改变既有 Fact → Lens → Act、工作空间隔离、Ray 恢复和 Photon 只追加边界：

- 主色从早期高饱和橙色统一为棕橙 `#8b5229`，主选中面为 `#f7efe5`，边界为 `#ebdcd0`；
- 设置开关使用 `#c86a28`，主表面采用暖白 `#f9f9f8 / #fbfbf9 / #fdfbf7`，卡片大圆角统一为 16px；
- 设置搜索移动到弹窗顶栏中央，左侧八页导航、中央表单、右侧配置概览和底部操作区保持真实可操作；
- 对话页在最终回答下方增加“执行工作流”卡片。它不是静态步骤，也不伪造流程，而是只投影当前 `user.input` 之后已经提交的 `model.*`、`act.*`、`tool.result`、`assistant.message` 和 `workflow.*` Photon；
- 轨迹页按新版设计改为统计卡、Input/Model/Tools 时间条、追加事件表和 Request 摘要四部分，数据来自同一 Ray 的完整 Surface；
- 对话工作流可独立折叠，轨迹页始终保留完整审计事件，两者都没有编辑、撤销或覆盖 Photon 的入口。

新版 Figma 基线：

| 页面 | node |
| --- | --- |
| 主界面 | `1:3` |
| 轨迹 | `1:907` |
| 通用设置 | `1:2043` |
| 智能体与模型 | `2:3360` |
| 权限与安全 | `2:4447` |
| 工作区 | `2:5526` |
| 通知 | `2:6601` |
| 外观 | `2:7673` |
| 快捷键 | `2:8765` |
| 账户 | `2:9869` |

## 2. 实际配置

验收文件位于：

```text
E:\cc\AI\tokmon\tokmon_work_test\test2\.tokmon\config.yaml
```

模型配置为：

```yaml
models:
  default: opencode
  goes:
    opencode:
      protocol: openai-compatible
      endpoint: https://opencode.ai/zen/v1/chat/completions
      model: x-preview-f-free
      secret_env: OPENCODE_API_KEY
      auth: bearer
      enabled: true
      allow_anonymous: false
      thinking: false
      reasoning_effort: high
      max_output_tokens: 4096
      max_attempts: 6
      retry_backoff_ms: 5000
```

没有写入 `api_key`、`secret` 或模型 `secret_ref` 字段。当前实现对每个新请求优先读取内部 ID `model-secret-library/opencode` 对应的 Credential Manager 记录，不存在时才读取 daemon 当前进程中的 `OPENCODE_API_KEY`；环境变量不会自动导入或覆盖保险库。Desktop/CLI 更新 Key 后下一次请求立即生效，外部修改系统环境变量仍需重启 daemon 才能改变其进程环境。

模型拒绝 `medium` 推理强度并返回 HTTP 400 后，配置改为该模型支持的 `high`；Desktop 的中文“低/标准/高/最高”在提交前统一归一化为 `low/medium/high/max`。

## 3. 真实模型验证

### 3.1 CLI 成功样本

执行：

```powershell
tokmon --workspace E:\cc\AI\tokmon\tokmon_work_test `
  --deadline-ms 120000 model test opencode "只回复：TOKMON_OPENCODE_OK"
```

真实返回 `assistant.message = TOKMON_OPENCODE_OK`，并追加模型 usage（输入 96、输出 63）。`model list` 显示 `opencode / x-preview-f-free / ready`。

### 3.2 Desktop 成功样本

通过 Desktop 输入框填入内容，再使用真实 Win32 鼠标点击发送按钮提交：

```text
Reply exactly: TOKMON_DESKTOP_EVENT_FLOW_OK
```

界面真实显示 `TOKMON_DESKTOP_EVENT_FLOW_OK`，完成任务 `1/1`；对话页保留人类可读步骤，轨迹页显示同一 Ray 的完整事件、时间条、provider/model、usage 与结果。

### 3.3 上游 503 样本

后续 CLI 与 Desktop 请求遇到 OpenCode 上游：

```text
HTTP 503: Upstream request failed: Endpoint is unavailable.
```

Rhea 现在按 `max_attempts: 6` 执行 1 次初始请求和 5 次重试，确定性退避为 `5s → 10s → 20s → 40s → 60s`。每次等待前追加 `model.retry-scheduled`，第六次请求仍失败后才追加最终 `model.failed` 与 `act.failed`。这说明认证、端点选择和请求发出均已发生；失败来自外部 endpoint 可用性。UI 的对话区显示本轮人类可读事件流，轨迹页保留完整不可变因果证据，最终状态变为“执行失败”。

## 4. 分组、项目与会话

### 4.1 导航数据模型

每个导航节点保存：

```yaml
- id: session_...
  ray: ray_...
  workspace: "" # 空值表示继承父项目；覆盖时为绝对路径
  kind: session
  title: 新会话 8
  indent: 2
  selected: true
  expanded: true
```

`kind` 仅允许 `group`、`project`、`session`。项目的 `workspace` 是规范化绝对路径；会话为空时沿树向上继承项目路径，非空时覆盖项目；分组不能绑定工作空间。标题、ID、Ray、路径长度、路径类型、缩进和总节点数都有边界验证。保存请求固定进入导航工作空间的 `navigation.save`，不会因为当前活动项目已切换而把全局导航树写错目录。tokmond 使用临时文件和原子替换更新 `ui.navigation`，随后追加 `ui.navigation.changed` Photon；不会编辑既有 Photon。

### 4.2 工作空间切换语义

Desktop 启动参数 `--workspace` 指定导航工作空间。导航树保存在该目录的 `.tokmon/config.yaml`，而模型、Ray、文件工具与项目配置使用当前选中项目/会话的活动工作空间：

```text
选择项目/会话
  → 解析项目 workspace 或会话 override
  → 规范化并创建目录
  → 按 workspace hash 计算 Snow endpoint
  → 必要时自动启动 tokmond 并附着 Desktop 租约
  → 发布新的活动 endpoint
  → 载入该工作空间配置与 provider
  → 新建空 Ray 或 surface(已有 ray)
  → 释放上一个活动工作空间租约
```

新 daemon 和租约完全建立之后才释放旧活动租约，启动失败不会把用户留在无可用 endpoint 的半切换状态。导航 daemon 在 Desktop 存活期内持续持有，因此在任意项目中都能保存树；这不是跨工作空间共享模型状态或 Photon。

### 4.3 原生 UI 操作结果

| 操作 | 现场结果 |
| --- | --- |
| 新建分组 `UI验收分组` | 插入根节点并写入 YAML |
| 在分组下新建项目 `UI验收项目` | 正确缩进并写入 YAML |
| 在项目下新建会话 | 自动编号、选中、清空当前投影 |
| 创建项目并指定 `workspace_alt` | YAML 保存规范化绝对路径，自动启动该目录的隔离 tokmond |
| 项目下创建继承会话 | `workspace: ""`，选择时使用父项目 `workspace_alt` |
| 创建覆盖会话并指定 `workspace_override` | YAML 保存覆盖路径并切换到新的隔离 endpoint |
| 覆盖会话 → 项目 → 继承会话 | override daemon 自动退出；项目 daemon 启动；继承会话保持同一 endpoint |
| Desktop 中执行 `/config` | 返回 `workspace_alt/.tokmon/config.yaml`，证明命令进入选中节点的真实工作空间 |
| 标题改为 `UI验收会话标题` | 树与标题栏同步，YAML 持久化，并执行 `/rename` |
| 搜索 `UI验收项目` | 只显示命中项目及其父分组 |
| 折叠 `UI验收分组` | 子节点隐藏，YAML 出现 `expanded: false` |
| 重启 Desktop | 分组、项目、会话、标题和选择状态恢复 |
| 会话 A → 新会话 B → 会话 A | 会话 A 的 Ray 与 Photon 轨迹重新投影 |

此前“新会话仅在内存中清空”“切换树节点不恢复对话”“设置保存会丢失导航”等问题均已修复。

## 5. Desktop UI 验收矩阵

所有项目均在 1440×900 逻辑像素、2160×1350 物理像素的真实 Slint 窗口中用 Win32 鼠标、键盘、剪贴板和窗口枚举完成，不是静态截图断言。

| 能力 | 验证动作 | 结果 |
| --- | --- | --- |
| 无边框窗口 | 自绘标题栏、最大化、关闭 | 最大化生效；关闭回调直接退出 Slint 事件循环 |
| 左右面板 | 关闭并从标题栏恢复 | 中央布局自动扩展/收缩 |
| 对话/轨迹 | 点击两个页签 | 对话和 Photon timeline 独立切换 |
| 代码/文件 | 点击代码审阅、文件预览 | 真实当前投影/结果状态；清除旧 `transcribe.py` 演示内容 |
| 文件菜单 | 打开投影/预览菜单 | 可切换，不再使用伪造文件名 |
| 问题面板 | 点击底部问题入口 | 无问题时显示 0，不伪造两条诊断 |
| 附件文件 | “+ → 添加文件” | 出现原生 `#32770` 文件对话框，标题为“选择要交给 Tokmon 的文件” |
| 权限/模型/强度 | 打开选择菜单 | 权限三项；模型由 tokmond 动态返回；强度四项 |
| 模型菜单 | 打开模型列表 | `local · local-deterministic` 与 `opencode · x-preview-f-free` |
| 斜杠命令 | 输入 `/st` | 自动匹配 `/status`、`/usage`、`/history`，点击回填后执行 |
| 设置弹窗 | 逐一点击八个设置页 | 通用、模型、权限、工作区、通知、外观、快捷键、账户均可操作 |
| 设置取消 | 修改自动保存后取消 | YAML SHA-256 不变 |
| 设置保存 | 修改后保存再恢复 | YAML 原子更新，provider 与 navigation 均保留 |
| Provider 测试 | 点击“测试真实连接” | 真实请求进入同一 Fact → Lens → Act 光路 |
| 文件/代码状态 | 新会话无工具修改 | 显示“当前会话尚无文件变更”，不展示伪造 Python |
| 当前回合工作流 | 真实鼠标发送文件写入、回读与命令验证任务 | 对话页只显示 Agent 的分析、写文件、回读、命令及验证结果，不再显示请求派发、Act 生命周期或透镜日志 |
| 新版轨迹页 | 切换到“轨迹” | 完整保留 70 个 Photon/Act/模型/工具事件、1m17s、7 次模型调用、4,671 tokens，并展示真实时间条和事件分类 |
| 八个新版设置页 | 分别以设置页 0..7 打开并检查特征控件 | 通用、模型、权限、工作区、通知、外观、快捷键、账户均成功渲染并可操作 |
| Desktop 生命周期复验 | 自绘关闭按钮与 `WM_CLOSE` 两种方式退出 | Desktop 退出后 `tokmond` 进程数归零 |

## 6. 本轮修复清单

1. 增加 `secret_env` schema、验证、合并和 Windows 环境引导；
2. Desktop provider/model 菜单改为 tokmond 动态数据；
3. UI 推理强度真实覆盖 model request，并归一化协议值；
4. 文件和文件夹入口接入 Win32 原生选择器；
5. 新建分组/项目/会话、重命名、选择和折叠写入项目 YAML；
6. 设置保存保留 `ui.navigation`，取消重新加载磁盘值；
7. 会话保存 Ray，切换或重启时通过 `surface(ray)` 恢复；
8. 新会话清除 timeline、code、assistant、user text 和状态；
9. 删除启动全局 snapshot 与 500 ms 全局尾重放，避免跨工作区/会话污染；
10. 清除 faster-whisper、音频路径、字幕、`transcribe.py`、伪诊断和伪统计；
11. 对 `act.failed`/`model.failed` 在对话和状态区显式显示失败；
12. Win32 窗口选择排除无标题的 Winit proxy，拖动和窗口控制指向真实 HWND；
13. 关闭按钮直接结束 Slint event loop，确保析构与租约 detach 执行；
14. Desktop 关闭后 tokmond 自动退出已现场验证。
15. 项目/会话创建弹窗增加工作空间输入、原生文件夹选择与错误提示；
16. 导航 YAML 增加 `workspace`，完成旧数据向项目默认工作空间的兼容迁移；
17. Desktop controller 分离“导航 endpoint”和“活动 endpoint”，实现跨工作空间原子切换与租约交接；
18. 会话空路径继承父项目，显式路径覆盖；Ray 始终与其工作空间一起切换；
19. 现场验证 `workspace_alt`、`workspace_override` 两个隔离 daemon 的启动、交接和 Desktop 关闭后清理。
20. 对话区新增当前回合 Photon 工作流投影，模型返回后不再只显示一段最终文本；
21. 轨迹页替换为新版统计、时间条、事件表与 Request 摘要布局，并绑定真实 usage/provider/model/outcome；
22. 主界面和八个设置页迁移到新版暖白、棕橙主题，设置搜索移入顶栏；
23. Windows 启动后再次强制清除原生 caption style，确保自绘标题栏是唯一窗口标题栏。
24. 对话工作流由原始 Photon 日志改为 Agent 语义事件；模型请求、派发、重试和 Act 生命周期只保留在轨迹页；
25. 任务完成必须同时满足工具调用、可验证结果和结果后的最终回复，纯文本承诺不再误标 `1/1` 完成；
26. Janus/Rhea/Techor 接通真实 `write_file`、`read_file`、`run_command` 和 `calculate` 工具链，并向后续模型轮次携带完整的已验证行动账本；
27. 模型失败采用 5/10/20/40/60 秒五次确定性重试，外层 Act 截止时间覆盖完整请求与等待预算；
28. OpenCode 含工具请求改为非流式协议，并将首包窗口对齐到 60 秒，避免工具规划在 10 秒被误切断。

## 7. 构建与自动化测试

```powershell
cmake --build build/windows-msvc-ui-debug --config Debug --parallel
ctest --test-dir build/windows-msvc-ui-debug -C Debug --output-on-failure
```

结果：构建成功，CTest `3/3` 测试入口通过，0 失败，最终完整回归耗时 12.80 秒。覆盖 append-only Photon store、20 个内置透镜、Janus Agent 行动账本、Rhea HTTP retry、Snow、MCP、LSP、PTY、Node.js、CPython、Git、RAG、凭据绑定和 daemon 租约等路径。

CLI 冒烟结果：

- `doctor`：storage verified；
- `lens list`：20 行；
- `model list`：local 与 opencode 均 ready，opencode 为默认；
- `chat /status`：tokmond healthy、LightPath 20 Lenses、provider opencode。

## 8. 安全审计

对项目级与用户级 `.tokmon` 下的 YAML、日志、JSON/JSONL/TXT 文本执行实际 Key 等值扫描：

```text
PLAINTEXT_SECRET_MATCHES=0
YAML_PLAINTEXT_KEY_FIELD=False
SECRET_ENV_REF=True
```

模型轨迹中的 credential 显示为 `<redacted>`。配置和 provider 列表只公开 `secret_env` 名称、`credential_present` 和 `credential_source`，不公开值或内部密钥 ID。

## 9. UI 证据

截图目录：`build/ui-acceptance/`。主要证据：

| 文件 | 内容 |
| --- | --- |
| `15-group-project-session.png` | 分组、项目与会话创建 |
| `16-slash-popup.png` | `/st` 自动匹配 |
| `20-settings-model-test.png` | Desktop 真实 provider 测试 |
| `23-rebuilt-startup.png` | YAML 导航重启恢复 |
| `25-new-session-idle-fixed.png` | 新会话空闲不被旧快照覆盖 |
| `26-real-api-rebuilt.png` | Desktop 真实 OpenCode 成功返回 |
| `29b-session-a-trajectory.png` | 跨会话恢复 Ray 轨迹 |
| `30-tree-search.png` | 项目搜索与父分组保留 |
| `31b-tree-collapsed.png` | 树折叠 |
| `32-attachment-menu.png` | 文件/文件夹入口 |
| `33-right-panel-closed.png` | 右面板关闭与重排 |
| `37-file-preview-issues.png` | 文件预览与问题区域 |
| `E:\cc\AI\tokmon\tokmon_work_test\desktop-new-theme-trace.png` | 新主题轨迹页、真实时间条、事件表与 Request 摘要 |
| `E:\cc\AI\tokmon\tokmon_work_test\desktop-new-theme-settings-account.png` | 新主题设置弹窗、顶部搜索、账户页和右侧概览 |
| `E:\cc\AI\tokmon\tokmon_work_test\desktop-new-theme-frameless.png` | 无边框自绘标题栏与完整新主题工作台 |
| `build/ui-qa-current/desktop-real-task-final.png` | 真实 Agent 写入、回读、命令验证和完成判据的对话工作流 |
| `build/ui-qa-current/desktop-real-task-trace.png` | 同一 Ray 的完整轨迹统计、时间条与 70 项事件明细 |

## 10. 外部状态与工具链

验收期间 OpenCode endpoint 曾返回 HTTP 503；Tokmon 实际记录了 5 秒和 10 秒重试后恢复，并完成同一真实任务。成功 Ray 为 `ray_01a033df32bd00000058bfa1ba1d09a61c31`：写入 24 bytes、回读 SHA-256 校验、`cmd.exe` 存在性检查输出 `FILE_EXISTS` 且退出码为 0，最终精确回复 `TOKMON_REAL_WORKFLOW_OK`。

```powershell
tokmon --workspace E:\cc\AI\tokmon\tokmon_work_test `
  --deadline-ms 120000 model test opencode "只回复：TOKMON_FINAL_CLI_OK"
```

本轮不需要用户补齐 Rust 工具链；Slint C++ 依赖已经成功生成并链接 Desktop。
