# Tokmon 斜杠命令完整使用说明

> 适用版本：Tokmon 0.1.0（C++20）  
> 交互面：`tokmon chat` 与 `tokmon-desktop`  
> 架构语义：A Lens to Them All；Fact → Lens → Act；因果光子流只追加

## 1. 命令是什么

斜杠命令是 Snow 接收的一类结构化意图。它不是绕开智能体和透镜的隐藏后门，也不是客户端直接修改运行时状态的快捷函数。CLI 与 Desktop 只负责收集输入和展示结果，`tokmond` 是唯一的事实提交者。

一条命令的标准因果链如下：

```text
用户输入 /command arguments
        │
        ▼
Snow: command.invoked Fact
        │
        ▼
Snow Lens: command.invoke Act
        │
        ▼
command.observed Fact
        │
        ├─ 只读折叠：history / status / usage / tasks …
        ├─ 专用 Lens Act：compact / diff / export / fork / skills …
        └─ 智能体光路：plan / review / security-review / debug
        │
        ▼
command.completed 或 command.failed Fact
```

每个 Act 仍由公共 ActPipeline 定位目标透镜，并经过 Fallen 权限裁决。客户端不能提交 Photon，也不能通过命令参数伪造批准。

## 2. 两种交互面的行为

### 2.1 CLI

进入交互模式：

```powershell
tokmon chat
```

输入 `/help` 查看完整目录，输入 `/help compact` 查看单条命令。命令与普通对话可以在同一会话交替使用：

```text
> 请检查当前项目
> /usage
> /compact 只保留与构建失败有关的上下文
> 继续修复
```

命令必须出现在输入开头。`请执行 /status` 是普通对话，不会被解释为命令。

### 2.2 Desktop

在 Desktop 输入框键入 `/` 会自动弹出命令匹配窗口：

- 输入 `/sec` 时只保留匹配的 `/security-review`；
- 每项显示用法、说明与类别；
- 鼠标悬停会高亮，点击会把规范命令名回填到输入框；
- 补充参数后按 Enter 或点击发送按钮执行；
- 输入第一个空格后匹配窗口自动关闭；
- 命令结果投影到对话区和轨迹区；
- `/config` 会直接打开设置弹窗；
- `/copy` 会把结果写入系统剪贴板；
- `/exit` 会关闭 Desktop，随后按租约规则关闭它拥有的 `tokmond`。

CLI 和 Desktop 使用同一个 C++ 命令目录与同一个解析器，因此不存在某一端“看得见但不能执行”的命令。

## 3. 参数规则

- 命令名不区分 ASCII 大小写，文档统一使用小写。
- 参数以空白分隔。
- 含空格的单个参数可使用单引号或双引号。
- 反斜线可转义下一个字符。
- `<参数>` 表示必填，`[参数]` 表示可选。
- 未知命令返回 `not_found`，不会退化成普通模型提示词。
- 缺失活动会话时，需要 Ray 的命令返回 `invalid_state`。

示例：

```text
/rename "修复 Windows 生命周期"
/fork '只读审查 apps/tokmond/main.cpp'
/rewind 1842
```

## 4. 完整命令目录

当前只保留与本地代码智能体、会话、透镜组合、模型和诊断直接相关的 33 条规范命令。账户、套餐、云端促销及厂商专属入口不进入 Tokmon 命令面。

### 4.1 帮助

#### `/help [command]`

显示全部命令，或显示指定命令的用法、说明和别名。

```text
/help
/help rewind
```

别名：`/commands`。

### 4.2 会话与因果历史

#### `/clear`

清空客户端当前选择，下一条普通消息创建新 Ray。原 Ray 及其全部 Photon 保持不变。

别名：`/new`、`/reset`。

#### `/exit`

关闭当前交互客户端。Desktop 会释放桌面租约；交互 CLI 会释放交互租约。若没有其他客户端且守护进程未被显式固定，`tokmond` 会按空闲关闭策略退出。

别名：`/quit`。

#### `/status`

显示 `tokmond` 健康状态、当前 LightPath epoch 和透镜数量、默认模型平台、Photon 全局尾序号与当前活动 Ray。

#### `/history [ray-id]`

显示指定 Ray 的已提交 Photon；省略参数时显示当前 Ray。没有当前 Ray 时显示全局历史。默认文本视图限制为最近 80 条，但响应中的结构化 Photon 仍可供 Desktop 投影。

#### `/resume <ray-id>`

将一个已有 Ray 设为当前会话。后续消息使用 `submit_to` 继续追加，不复制也不改写历史。

#### `/rename <名称>`

向当前 Ray 追加 `session.renamed` Fact。旧标题事实仍保留，UI 只折叠显示最新标题。

#### `/branch [名称]`

从当前因果尾创建新 Ray，并在父、子两侧分别追加 `branch.created` 与 `ray.branched`。新 Ray 成为当前会话。

#### `/rewind <sequence>`

“回到”指定序号的含义是从该检查点创建新分支：验证序号属于当前 Ray；创建新 Ray；记录父 Ray、源序号与 `history_deleted: false`。它不会删除、编辑或撤销源序号之后的任何 Photon。

#### `/copy [数量]`

提取最近一条或多条 `assistant.message`。Desktop 写入系统剪贴板；CLI 把可复制文本输出到终端。

#### `/export [文件]`

把当前 Ray 渲染为 Markdown：Cove 先执行 `artifact.create` 生成不可变、内容寻址的中间制品，再执行 `artifact.export` 写到工作区内的相对路径，两次 Act 都进入审计流。

默认文件名为 `tokmon-session-<ray>.md`。目标已存在时会拒绝覆盖；请提供新文件名。

### 4.3 上下文

#### `/context [all]`

统计用户输入、助手消息、工具结果、Textus 摘要、Photon 数和配置的窗口上限。`all` 统计全局因果流；省略时统计当前 Ray。

#### `/compact [关注点]`

向 Textus 提交 `text.compact` Act，追加 `summary.created`。摘要带覆盖序号、窗口哈希和源 Photon 引用；原始上下文继续保留。

### 4.4 模型与运行时

#### `/model [平台-id]`

无参数时列出启用的平台及模型。带平台 ID 时更新项目级 `.tokmon/config.yaml` 的默认平台并热重载运行时。

平台配置和 API Key 的首次录入仍使用 Desktop 设置页，或 CLI 顶层命令：

```powershell
tokmon model configure deepseek `
  --protocol openai-compatible `
  --endpoint https://api.deepseek.com/chat/completions `
  --model deepseek-chat `
  --default
tokmon model secret set deepseek
tokmon model test deepseek
```

Rhea 只依赖协议适配，不与平台品牌绑定。

#### `/effort [low|medium|high|max]`

设置当前客户端会话的推理强度。Desktop 同步更新输入区强度菜单；后续模型上下文携带该值。

#### `/permissions [full|restricted|read-only]`

设置当前客户端的访问模式并同步 Desktop 菜单。该设置是会话约束，不会绕过 Fallen 对每个 Act 的最终裁决。

#### `/config`

CLI 输出项目级和用户级 YAML 路径；Desktop 打开设置弹窗。别名：`/settings`。

#### `/usage`

折叠 `model.usage`，显示模型调用次数、输入/输出 token、成本微单位和 Photon 数。别名：`/cost`、`/stats`。

### 4.5 智能体

#### `/plan <任务>`

把任务转为明确的规划提示，使用当前平台和会话约束进入完整智能体光路，并返回实际 `assistant.message`。它不是本地静态模板。

#### `/tasks`

从 `act.*`、`tool.*`、`ray.*` Photon 折叠当前任务和步骤。没有步骤时明确返回空状态。

#### `/agents`

从 `child.*` Photon 折叠父子 Ray、进度、用量、完成或失败状态。

#### `/fork <任务>`

通过 Aya 执行 `child.spawn`：模式为 `fork`，默认只读工作区，子预算不超过父预算的一半，不继承秘密材料，允许的 ActKinds 显式写入参数，并返回可独立寻址的 Child Ray。

别名：`/subtask`。

### 4.6 工程工作流

#### `/diff`

通过 Cove 执行真实 `git status --porcelain=v2 --branch --untracked-files=all`，不启动 shell，不修改 Git 状态。非 Git 工作区会产生可审计的 `command.failed`。

#### `/review [目标]`

使用当前模型和可用透镜执行代码审查。省略目标时审查当前工作区。

#### `/security-review [目标]`

执行按风险分级、要求证据的安全审查。它沿智能体光路工作，因此工具调用仍受 Fallen 和各透镜边界约束。

#### `/debug [问题]`

让智能体基于当前会话和可用诊断透镜分析问题。省略参数时以当前会话异常为目标。

#### `/init`

确认项目级 `.tokmon` 目录约定已就绪，并追加 `project.initialized` Fact。重复执行是可审计且非破坏性的。

### 4.7 透镜生态

#### `/lenses [reconcile]`

无参数时列出 LightPath epoch、每个 Lens ID 和 generation；`reconcile` 重新读取 YAML、验证候选光路并原子发布新 epoch。

#### `/lens <list|reconcile>`

单数形式只暴露必要的 `list` 与 `reconcile`。挂载、替换和卸载继续由显式 CLI 顶层命令完成，避免在对话输入中意外扩大运行时变更范围。

#### `/skills [discover]`

无参数时显示已提交的技能发现事实；`discover` 通过 Enso 扫描项目根和用户 `.tokmon` 根中的 `SKILL.md`，并追加发现结果。

#### `/mcp`

显示 Iris 已提交的 MCP/连接事实。它是状态投影，不会隐式连接未知服务。

#### `/memory`

显示 `memory.accepted` 与相应失效事实。提议但未接受的内容不会伪装成长期记忆。

## 5. 别名表

| 规范命令 | 别名 |
|---|---|
| `/help` | `/commands` |
| `/clear` | `/new`、`/reset` |
| `/exit` | `/quit` |
| `/config` | `/settings` |
| `/usage` | `/cost`、`/stats` |
| `/fork` | `/subtask` |
| `/doctor` | `/checkup` |

别名在解析后会归一到规范命令名；`command.invoked` 同时记录实际输入名和规范名。

## 6. 权限、失败和幂等

观察型命令通常只折叠 Photon。触发 Act 的命令由 ActPipeline 和 Fallen 裁决。若项目策略要求批准，命令返回 `approval_required`，不会因来自 Desktop 而自动允许。

解析、校验、目标透镜、权限、I/O 或模型错误都会产生 `command.failed`，字段包含原始命令输入、稳定错误码和描述，以及 `history_deleted: false`。失败之前已经提交的 Photon 不回滚；后续可以在相同 Ray 继续追加修复行为。

Snow 的 `request_id` 仍使用守护进程已有的完成结果缓存。网络重试同一请求 ID 时返回原结果，避免重复执行同一命令。Lens Act 另有 `idempotency_key` 和绑定哈希审计。

## 7. 常用流程

### 7.1 新会话、规划、压缩

```text
/clear
/model deepseek
/effort high
/permissions restricted
/plan 修复全部失败测试并给出证据
/usage
/compact 仅保留失败原因、改动和验证结果
```

### 7.2 审查并从检查点分支

```text
/review apps/tokmond/main.cpp
/history
/rewind 1842
/rename 采用更保守的修复方案
```

序号 1842 之后的源历史仍然存在，新输入只追加到新 Ray。

### 7.3 组合子光线

```text
/fork 只读审查协议层并返回风险清单
/agents
/tasks
```

### 7.4 导出证据

```text
/doctor
/usage
/export reports/session-audit.md
```

## 8. 与 CLI 顶层命令的边界

斜杠命令面向交互会话。以下运维动作保留在显式 CLI 顶层，避免把高影响操作藏在对话中：

- `tokmon daemon start|stop|status`；
- `tokmon model configure|secret|test`；
- `tokmon lens mount|replace|unmount`；
- `tokmon stdio` 机器协议服务；
- `tokmon config paths`。

这种边界不削弱可组合性：所有交互命令仍可随 LightPath 的 Lens generation 热替换而组合变化；只是运维权限需要显式、可见的入口。

## 9. 实现位置

- 共享目录与解析器：`sdk/cpp/include/tokmon/slash_commands.hpp`、`protocol/slash_commands.cpp`
- 守护进程命令执行：`apps/tokmond/main.cpp`
- Snow 命令 Act：`lenses/snow/snow_lens.cpp`
- CLI 路由：`apps/tokmon-cli/main.cpp`
- Desktop 路由与匹配模型：`apps/tokmon-desktop/main.cpp`
- Desktop 悬浮菜单：`apps/tokmon-desktop/ui/tokmon.slint`

新增命令必须先进入共享目录，再在 Snow 执行器中实现，并同时满足 CLI、Desktop、Photon 审计和测试四个入口；禁止只在某一个客户端增加私有命令。
