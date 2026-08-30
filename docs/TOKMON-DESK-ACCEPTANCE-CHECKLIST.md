# tokmon-desk 完整重写验收清单（Browser 暂缓版）

> 版本：2026-08-30  
> 对应架构：[DESKTOP-RMLUI-FINAL-ARCHITECTURE.md](DESKTOP-RMLUI-FINAL-ARCHITECTURE.md)  
> 新实现目录：`apps/tokmon-desk`  
> 旧版视觉与交互主基线：`apps/tokmon-desktop`  
> 原型辅助基线：`E:/cc/AI/tokmon/Tokmon UI`  
> 端到端测试工作区：`E:/cc/AI/tokmon/tokmon-desk-work-test`

## 1. 验收结论规则

本清单是 `tokmon-desk` 本轮发布验收的唯一执行清单。它把“已存在代码”“可演示”和“验收通过”严格区分：只有自动测试、视觉证据、安装态验证和人工复核均满足对应条目时，条目才可勾选。

- `[ ]`：未验收，不能据此声称完整实现。
- `[x]`：已有可复现证据，证据路径或命令必须记录在第 16 节。
- `DEFERRED-BROWSER`：本轮经明确批准暂缓，只适用于第 13 节，不计入本轮通过率，也不得掩盖共享模块回归。
- `BLOCKED-PLATFORM`：当前机器无法运行目标平台测试；只能记录静态检查，不得写成目标平台已通过。
- P0 条目任何一项未通过，本轮总结果必须是“不通过”。
- P1 条目任何一项未通过，除非本文预先标记为建议项，本轮总结果仍为“不通过”。
- 旧版 UI 复刻是 P0 发布硬门槛，不能以功能、性能或技术栈完成度抵消。

本轮通过定义：除第 13 节 Browser 外，所有 P0、P1 必须有证据并通过；Windows 必须完成 Release 安装态端到端测试；macOS/Linux 在当前 Windows 主机上只能完成源代码、CMake 和接口静态验收，真实平台构建与运行结果必须明确保留为 `BLOCKED-PLATFORM`，不能虚报。

### 1.1 2026-08-30 实际执行快照

- 总结论：**旧版主工作台视觉复刻的自动验收已通过；完整发布验收仍未签署**。剩余发布阻塞项是合规的同步数据 golden 像素阈值、用户人工签署、真实 IME/device-lost，以及 macOS/Linux 真机构建；这些阻塞不再包含本轮截图中暴露的尺寸、三栏结构、导航密度、右栏启动器或透明度问题。
- Windows 自动化：最终构建 CTest 2/2；主 DPI UI 37/37；五档 DPI 累计 185/185；三视口 × 五 DPI 几何 15/15；最终样式重新生成 26/26 张核心状态图；安装态 UI 37/37。
- 视觉主基线：Windows 150% 显示缩放 × 旧版 125% 内容缩放，物理输出 `2472×1688`，RmlUi 内容视口 `1318×900`；三栏几何 contract 9/9。
- 压力：10,000 会话、10,000 文件模型、100,001 行编辑器、4,101 行 diff、100 MiB Terminal 均通过 DOM/耗时硬门槛；最终输入到 Skia present 为 64 样本、p95 7.6648 ms。
- 安装态：157 个文件、20 个 runtime DLL、49 个 license 文件；无开发头、静态库、Debug CRT、完整 Ghostty 或 Browser runtime；构建态/安装态 exe SHA-256 一致。
- 保护边界：`git diff -- apps/tokmon-desktop apps/tokmond` 为空。
- Browser：保持 `DEFERRED-BROWSER`，未执行 Browser 测试。
- 机器报告：[acceptance-summary.json](../build/windows-msvc-desk-strict-release/e2e/acceptance-summary.json)。

### 1.2 本轮“截图完全不一样”问题的纠正结论

本轮不是对原 UI 重新设计，而是纠正了此前错误的运行基线和实现偏差。根因与修复如下：

| 根因 | 旧错误表现 | 已完成修复 |
|---|---|---|
| 默认窗口仍按 `1440×900` 启动 | 整套界面比旧版小一圈 | 恢复旧版实际 `1648×1125` 窗口逻辑尺寸，在本机输出 `2472×1688` |
| 显示缩放与内容缩放被错误合并/覆盖 | 显式 DPI 参数会把旧版 125% 内容缩放重置 | 两者独立；最终有效比例 188%，RmlUi 视口 `1318×900` |
| 右栏沿用 440px 常驻功能页 | 中间工作区被压窄，和旧版启动器状态不符 | 默认 214px 旧版“审查/文件”启动器；功能页窄栏响应式，打开文件后才扩宽 |
| 未恢复旧版内置导航示例 | 左栏只有 `bin/新会话`，视觉密度和层级完全错误 | 一次性迁移旧 Slint 的 11 条内置分组/项目/会话，同时保留现有真实导航 |
| 工作区恢复失败 | 标题和路径落到 `bin` | Desktop 私有设置持久化 `last_workspace`，并从导航状态兼容恢复 |
| RCSS `rgba()` 使用了浏览器 0–1 alpha | hover、半透明 pill 和 modal 遮罩近乎透明 | 按 RmlUi 规范改为 0–255 alpha；遮罩、hover、边框透明度恢复 |
| RmlUi 预乘色直接传给 Skia | 半透明彩色控件被二次预乘而过暗 | RenderInterface 转为非预乘 `SkColor` 后再交给 SkVertices |
| 右栏/搜索/composer 缺少旧版紧凑规则 | 文案溢出、文件页挤成一团、发送态错误 | 恢复 placeholder、禁用发送、旧 SVG、214px 窄栏和文件打开自动扩宽 |

旧版截图与最终新版的同尺寸并排证据：

- `build/windows-msvc-desk-strict-release/e2e/legacy-vs-tokmon-desk-final.png`
- 左半为用户提供的旧版真实截图，右半为最终 `tokmon-desk` D3D12/Skia 截图；两边均为 `2472×1688`。

#### 主工作台视觉复刻专项验收

| ID | 验收点 | 最终证据 | 状态 |
|---|---|---|---|
| CLONE-001 | 物理尺寸、显示缩放、内容缩放与旧版一致 | `ui-contract-final.json`：2472×1688 / 150% / 125% / 1318×900 | [x] |
| CLONE-002 | 左栏 240、右栏 214、工作区 864、标题栏 46、composer 760×100 | 9/9 元素在 1 Rml 逻辑像素容差内 | [x] |
| CLONE-003 | 品牌、新建、搜索、树层级、底部设置复刻 | `visual-clone-final.png` 与最终 26 图集 | [x] |
| CLONE-004 | 标题、对话/轨迹、环境 orb、starter 主区复刻 | 同尺寸并排图；环境开关真实点击通过 | [x] |
| CLONE-005 | 四张 starter 卡片的尺寸、色彩、图标、文本层级复刻 | 冻结 SVG 65/65；最终截图 | [x] |
| CLONE-006 | composer 上下文条、输入卡、权限/模型/effort/发送态复刻 | 空输入禁用与权限菜单 E2E 通过 | [x] |
| CLONE-007 | 右栏旧版启动器和快捷键 pill 复刻 | 启动器打开 Review/Files 与折叠恢复 E2E 通过 | [x] |
| CLONE-008 | hover/pressed/focus/selected/disabled/loading/success/warning/error | `visual-state-gallery-visual-clone-final/`，26/26 | [x] |
| CLONE-009 | DPI 与命中一致 | 100/125/150/175/200：185/185；3 视口 × 5 DPI：15/15 | [x] |
| CLONE-010 | Codex 同尺寸并排视觉复核 | `legacy-vs-tokmon-desk-final.png` | [x] |
| CLONE-011 | 用户最终人工视觉签署 | 需用户查看最终并排图确认 | [ ] |

## 2. 范围与不可突破边界

| ID | 级别 | 验收项 | 判定方法 | 状态 |
|---|---|---|---|---|
| SCOPE-001 | P0 | 新版产品名、可执行文件名、进程名均为 `tokmon-desk` | 安装目录、进程与窗口检查 | [x] |
| SCOPE-002 | P0 | 所有新版 Desktop 实现位于 `apps/tokmon-desk`；旧 `apps/tokmon-desktop` 不修改、不删除 | `git diff -- apps/tokmon-desktop` 必须为空 | [x] |
| SCOPE-003 | P0 | `tokmon-daemon` 保持现有职责，不为 Desktop UI、编辑器、Git Review、Terminal 或 Browser 新增 RPC | `git diff -- apps/tokmond` 及协议审计 | [x] |
| SCOPE-004 | P0 | Desktop 私有功能和状态全部在 Desktop 代码与 Desktop 数据目录内实现 | 路径测试、接口审计 | [x] |
| SCOPE-005 | P0 | 不使用 OpenGL/GL3；Windows D3D12、macOS Metal、Linux Vulkan | 构建源和链接依赖审计 | [x]（目标平台运行另见 PKG-009） |
| SCOPE-006 | P0 | 本轮不实现、不测试、不宣称 Browser 完成 | 第 13 节全部保持 `DEFERRED-BROWSER` | [x] |
| SCOPE-007 | P0 | Browser 暂缓不得导致其他右侧面板、设置或打包流程崩溃 | 非 Browser UI 回归测试 | [x] |

## 3. 旧版 UI 冻结基线

### 3.1 基线优先级

1. `apps/tokmon-desktop` 的实际运行效果、Slint 源码、资源和交互为唯一主基线。
2. `E:/cc/AI/tokmon/Tokmon UI` 仅用于补足旧版实现未覆盖或有已知 bug 的交互意图，不得覆盖旧版颜色、排版、尺寸和整体设计。
3. 架构文档用于约束技术实现和新增能力；不得借技术重写重新设计旧版 UI。
4. 允许的预先批准差异只有产品名 `tokmon-desk`、底层渲染造成且低于阈值的抗锯齿差异，以及新能力在旧版设计语言中的必要延伸。

### 3.2 基线完整性

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| BASE-001 | P0 | 记录旧版 Git commit、主题和主 UI SHA-256 | manifest 字段完整且与文件重算一致 | [x] |
| BASE-002 | P0 | 冻结 `tokmon-theme.slint` 的全部 Palette RGBA | 自动提取清单逐项精确相等 | [x] |
| BASE-003 | P0 | 冻结旧版全部 `icon-00.svg` 至 `icon-59.svg`、spinner 和 starter 图标 | 65 个资源逐文件 SHA-256 一致 | [x] |
| BASE-004 | P0 | 冻结 MiSans 实际字体文件、字号、字重和行高映射 | 字体 SHA-256 与 token 映射通过 | [x] |
| BASE-005 | P0 | 冻结旧版关键几何 token | 侧栏、标题栏、右栏、composer、卡片、按钮、弹窗等全部列入机器可读 manifest | [x] |
| BASE-006 | P0 | 新版运行时不读取或链接 `apps/tokmon-desktop` | 安装态脱离源码目录启动通过 | [x] |
| BASE-007 | P1 | 原型 commit、入口文件 hash 和访问地址只作为辅助记录 | manifest 可追溯 | [x] |

### 3.3 像素与几何判定

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| VIS-001 | P0 | 颜色 | 所有非抗锯齿实体色 RGBA 精确一致；不得近似替换 | [x]（冻结 Palette；RmlUi byte-alpha 与 Skia 预乘色已纠正） |
| VIS-002 | P0 | 几何 | 控件位置、宽高、间距、圆角、边框误差不超过 1 个逻辑像素 | [x]（15/15 几何 contract） |
| VIS-003 | P0 | 字体 | MiSans、字号、字重、行高、字间距与旧版 token 一致 | [x]（MiSans VF hash 与 token 校验通过） |
| VIS-004 | P0 | 图标 | 使用冻结 SVG，尺寸和对齐误差不超过 1 个逻辑像素 | [x] |
| VIS-005 | P0 | 非文本像素差 | golden 对比差异像素占比不超过 0.5% | [ ] |
| VIS-006 | P0 | 文本抗锯齿遮罩 | 只允许字形边缘的窄遮罩，不允许遮掉位置、字号、颜色或行距错误 | [ ] |
| VIS-007 | P0 | DPI | 100%、125%、150%、175%、200% 均无裁切、重叠、模糊缩放或命中偏移 | [x]（交互 185/185；几何矩阵 15/15） |
| VIS-008 | P0 | 视口 | 1280×800、1440×900、1920×1080 均通过；1440×900 为主基线 | [x] |
| VIS-009 | P0 | UI 不得过小 | 主基线下逻辑尺寸与旧版一致；不得用全局缩小伪装布局问题 | [x]（区分显示缩放与旧版 4K 125% 内容缩放） |
| VIS-010 | P0 | 状态图 | default、hover、pressed、focus、selected、disabled、loading、success、warning、error 均有证据 | [x]（10/10；另有 16 张页面状态） |
| VIS-011 | P0 | 人工并排复核 | 普通用户不能从视觉和任务路径判断底层已从 Slint 切换为 RmlUi/Skia | [ ] |

## 4. 旧版 UI 页面与交互复刻矩阵

以下每一行都要在 1440×900、150% DPI 主场景保存新版截图；标有“状态集”的页面还需保存对应状态截图。Browser 入口可保留禁用/暂缓提示，但 Browser 内容不在本轮验收。

| ID | 页面/区域 | 必须复刻的视觉 | 必须复刻的交互与状态 | 状态 |
|---|---|---|---|---|
| UI-001 | 无边框窗口壳 | 圆角/背景/三栏边界、拖拽区、窗口控件位置和图标 | 拖动、最小化、最大化/还原、关闭、双击标题栏 | [ ] |
| UI-002 | 左侧品牌区 | Tokmon 标志、名称、顶部留白和对齐 | 拖动区不吞掉可点击控件 | [ ] |
| UI-003 | 新建会话按钮 | 216×40、旧版米色、圆角、图标与文字 | hover/pressed/focus，打开新建弹窗 | [ ] |
| UI-004 | 导航搜索 | 搜索图标、placeholder、输入框背景和间距 | 输入过滤、清空、无结果、键盘焦点 | [ ] |
| UI-005 | 分组/项目/会话树 | 层级缩进、折叠图标、选中背景、计数和省略号 | 展开/折叠、选择、重命名、新建、上下文操作、过滤后层级保持 | [ ] |
| UI-006 | 左侧栏折叠 | 折叠按钮位置和左右箭头 | 折叠/恢复、窗口重启后恢复 | [ ] |
| UI-007 | 设置入口 | 底部固定栏、分隔线、16px 图标 | 打开设置、键盘可达 | [ ] |
| UI-008 | 工作区标题栏 | 标题、编辑图标、对话/轨迹 pill、环境入口 | 改名、切换模式、打开/关闭环境面板 | [ ] |
| UI-009 | 环境信息面板 | 工作区、分支、Daemon 状态、旧版颜色与间距 | 显隐、状态刷新、长路径截断/提示 | [ ] |
| UI-010 | 初始会话页 | 主标题、工作区名、四张 starter 卡片及原图标 | 四张卡片分别填入正确提示并聚焦 composer | [ ] |
| UI-011 | Composer 上下文条 | 工作区、本地、分支图标与旧版排版 | 工作区/分支更新后即时刷新 | [ ] |
| UI-012 | Composer 输入卡 | 输入区、圆形附件按钮、联网/权限 pills、模型/effort、context、发送按钮 | 输入、Enter 发送、空消息禁用、停止、附件、联网、权限、模型、effort popover | [ ] |
| UI-013 | Slash 命令 | 旧版浮层、选中样式、说明文字 | `/` 触发、过滤、方向键、Enter、Esc | [ ] |
| UI-014 | 会话消息 | 用户/助手消息排版、Markdown、代码块、工具状态 | 流式增量、复制、滚动跟随、手动离底不强制跳回 | [ ] |
| UI-015 | 轨迹页 | 空态、事件卡片、时间/因果层级样式 | 对话/轨迹切换、刷新、导出、错误事件 | [ ] |
| UI-016 | 右侧标题栏 | 添加标签、当前标题、快捷键、全屏/折叠/窗口控件 | 添加/选择/关闭标签，右栏全屏和折叠恢复 | [ ] |
| UI-017 | 右侧页签 | 审查/文件/终端的旧版图标、选中态与快捷键 | 点击和快捷键切换，状态保持 | [ ] |
| UI-018 | Review 空态 | 旧版图标、标题和说明 | 无 Git、干净仓库、加载、错误状态 | [ ] |
| UI-019 | Review 工具栏 | 分支、提交/推送、刷新按钮几何和状态 | 分支菜单、刷新、提交弹窗、推送结果 | [ ] |
| UI-020 | Review 更改列表 | 文件状态、数量、折叠、选中和 staged/unstaged 分组 | 文件选择、stage/unstage、全部 stage | [ ] |
| UI-021 | Unified Diff | 旧版暗色 diff、行号、hunk header、增删背景 | 虚拟滚动、hunk stage/unstage/discard、长行横向滚动 | [ ] |
| UI-022 | Split Diff | 左右栏、对应行、空白占位和行号 | unified/split 切换且保持文件与滚动语义 | [ ] |
| UI-023 | 放弃修改确认 | 危险色、文案、按钮层级 | 取消、hash 冲突阻止、确认、结果 toast | [ ] |
| UI-024 | 提交弹窗 | 标题、输入框、取消/提交/提交并推送 | 空消息验证、成功、失败、push 失败不丢提交结果 | [ ] |
| UI-025 | 文件页工具栏 | 搜索、路径、语法状态、撤销/重做/重载/保存 | 按钮 enable 状态、快捷键、保存反馈 | [ ] |
| UI-026 | 文件树 | 文件夹/文件图标、缩进、选中、hover、长名省略 | 异步展开、搜索、选择、刷新、重命名/新建/删除、键盘导航 | [ ] |
| UI-027 | 编辑器 | 旧版面板边界、行号、选区、光标、语法色、滚动条 | 输入、选择、复制粘贴、undo/redo、IME、横纵滚动、冲突处理 | [ ] |
| UI-028 | Terminal 工具栏 | 标签、加号、关闭、状态、搜索框和计数 | 多标签、新建/关闭/切换、搜索/清除 | [ ] |
| UI-029 | Terminal Surface | 终端背景、字体、16/256/真彩色、光标和选择 | 输入、TUI、滚动、选择复制、粘贴确认、resize、OSC 8 | [ ] |
| UI-030 | 多行粘贴确认 | 危险提示弹窗样式 | 安全文本直贴；多行/控制字符必须确认 | [ ] |
| UI-031 | 设置壳 | 左导航、搜索、标题说明、内容卡片、footer | 打开/关闭、导航、搜索过滤、保存、重置、滚动 | [ ] |
| UI-032 | 设置/通用 | 启动恢复、缩放、主题锁定等旧版布局 | 本地保存、重启恢复 | [ ] |
| UI-033 | 设置/智能体与模型 | Provider、模型、effort、错误与空态 | 读取共享业务配置、修改走既有 Daemon 能力 | [ ] |
| UI-034 | 设置/权限与安全 | 权限项和危险提示 | 更改、保存、失败回滚 | [ ] |
| UI-035 | 设置/工作区 | 默认工作区、保护和文件选项 | 目录选择、校验、保存 | [ ] |
| UI-036 | 设置/通知 | 开关与说明 | 保存、恢复 | [ ] |
| UI-037 | 设置/外观 | 旧版主题锁定、密度/字号/缩放 | 仅允许批准的缩放，不改变主题设计 | [ ] |
| UI-038 | 设置/快捷键 | 分组、键帽和冲突样式 | 搜索、冲突提示、恢复默认 | [ ] |
| UI-039 | 设置/账户 | 用户信息、登录/退出或本地状态 | 不可用状态明确，不能伪成功 | [ ] |
| UI-040 | 设置/终端 | shell profile、字号、scrollback、安全粘贴 | 跨平台 profile 保存并应用到新会话 | [ ] |
| UI-041 | 设置/关于 | 产品、版本、技术栈、许可证入口 | 可打开 notices/licenses；包含 HarfBuzz 等许可信息 | [ ] |
| UI-042 | 新建会话弹窗 | 标题、字段、项目下拉、错误和按钮 | 校验、取消、创建、重复名行为 | [ ] |
| UI-043 | Popover/Menu/Toast | 旧版圆角、阴影、色彩、层级和动画节奏 | 点击外部关闭、Esc、焦点返回、成功/警告/错误 | [ ] |
| UI-044 | 空态/加载/错误 | 各页面统一沿用旧版设计语言 | 重试、无数据、离线、权限不足 | [ ] |
| UI-045 | 键盘焦点 | 旧版视觉可辨识且不破坏设计 | Tab 顺序、Shift+Tab、Enter/Space、Esc | [ ] |

## 5. 渲染、字体、输入与可访问交互

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| REN-001 | P0 | SDL3 只负责窗口、事件、输入、DPI 和平台表面 | 依赖/代码审计通过 | [x] |
| REN-002 | P0 | RmlUi 负责 DOM、RCSS、布局和事件 | 页面不依赖 Slint runtime | [x] |
| REN-003 | P0 | Skia Ganesh 实现 RmlUi RenderInterface | clip、transform、texture、geometry、opacity 正确 | [x]（Windows D3D12） |
| REN-004 | P0 | Windows 使用 D3D12，macOS 使用 Metal，Linux 使用 Vulkan | 源码和 CMake 平台分支明确，无 GL3 | [x]（静态分支；真机见 PKG-009） |
| REN-005 | P0 | GPU device lost、resize、最小化、DPI 变化可恢复 | Windows 端到端场景通过；其他平台保留真实运行验收 | [ ] |
| FONT-001 | P0 | UI 字体优先 MiSans，回退链覆盖 CJK、emoji、符号和等宽字体 | 字体枚举与截图通过 | [ ] |
| FONT-002 | P0 | HarfBuzz shaping 与 FreeType glyph 管理 | 中文、英文、阿拉伯文、组合字符和 emoji 测试通过 | [ ] |
| FONT-003 | P0 | glyph atlas 支持增量缓存和 DPI 失效 | 压力测试无错字/花屏 | [ ] |
| INPUT-001 | P0 | SDL_TEXT_INPUT 与 IME composition 独立处理 | 中文预编辑、候选、提交、取消测试通过 | [ ] |
| INPUT-002 | P0 | caret/selection 按 grapheme cluster 移动 | 组合音标、ZWJ emoji、肤色、旗帜不被拆开 | [ ] |
| INPUT-003 | P1 | 鼠标、触控板、滚轮、键盘快捷键在缩放后命中正确 | 多 DPI 自动/人工测试 | [ ] |

## 6. Markdown 与长会话

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| MD-001 | P0 | MD4C 只作为 tokenizer/parser 基础，输出自有 AST | UI 不直接消费 MD4C callback/HTML | [x] |
| MD-002 | P0 | 每个 AST 节点包含稳定 NodeId、类型、源码 byte range、父子关系和属性 | 单元测试覆盖 | [x] |
| MD-003 | P0 | 支持标题、段落、引用、列表、任务列表、代码块、表格、分隔线、链接、图片、强调、删除线、行内代码 | fixture 全通过 | [x] |
| MD-004 | P0 | 支持 Tokmon 扩展：文件引用、diff、callout、tool call/result | AST 和安全 renderer 测试 | [x] |
| MD-005 | P0 | 流式消息只重解析未闭合尾块，已闭合节点 ID 稳定 | chunk 边界随机化测试 | [x] |
| MD-006 | P0 | 原始 HTML 默认按文本处理；URL scheme、文件路径和命令动作白名单 | XSS/协议/路径穿越 fixture 通过 | [x] |
| MD-007 | P0 | 代码块和超长 Markdown 不生成无界 RmlUi DOM | 100k 行/10MB fixture 有上限且可交互 | [x] |
| CHAT-001 | P0 | 会话消息虚拟化 | 10,000 轮对话 DOM 节点保持在配置上限内 | [x]（369 DOM nodes） |
| CHAT-002 | P0 | 流式滚动语义 | 位于底部自动跟随；用户上滚后不抢滚动 | [ ] |
| CHAT-003 | P1 | Markdown 复制保留纯文本语义，代码块可单独复制 | 交互测试 | [ ] |

## 7. 工作区、文件与编辑器

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| WS-001 | P0 | root 内路径规范化和 containment | `..`、symlink/junction、大小写和绝对路径逃逸全部拒绝 | [ ] |
| WS-002 | P0 | 文件树异步、懒加载、虚拟化 | 100k 文件 fixture 不阻塞 UI，DOM 有上限 | [ ] |
| WS-003 | P0 | 搜索异步、可取消、有代次保护 | 快速连续查询只显示最后一代 | [ ] |
| WS-004 | P0 | 遵循 `.gitignore` 和 Desktop 排除规则 | 嵌套规则、否定规则、目录规则测试 | [ ] |
| WS-005 | P0 | 新建、重命名、删除文件/目录均限制在 root 内 | 成功、冲突、权限失败、逃逸测试 | [ ] |
| WS-006 | P0 | 文件 watcher 合并抖动并区分自身保存与外部变化 | burst、rename、delete、external edit 测试 | [ ] |
| DOC-001 | P0 | 打开时识别 UTF-8/BOM、换行、二进制、大文件、只读 | fixture 测试 | [ ] |
| DOC-002 | P0 | 保存使用同目录临时文件、flush、原子替换，保留编码/BOM/换行 | 故障注入与恢复测试 | [ ] |
| DOC-003 | P0 | 外部修改时不静默覆盖 | reload/overwrite/cancel 冲突流通过 | [ ] |
| DOC-004 | P0 | undo/redo、dirty、保存点和恢复一致 | 随机编辑序列测试 | [ ] |
| DOC-005 | P0 | 崩溃恢复快照存 Desktop state，不污染项目 `.tokmon` | kill/restart 恢复测试 | [ ] |
| EDIT-001 | P0 | CodeSurface 不为每行建立 RmlUi DOM | 100k 行仅绘制可见行与 overscan | [x]（100,001 行；19 可见行；0 子 DOM） |
| EDIT-002 | P0 | 光标、选择、行号、横纵滚动、长行和 tab 宽正确 | 自动模型测试与截图 | [ ] |
| EDIT-003 | P0 | UTF-8/grapheme/IME 编辑不破坏文本 | Unicode fixture 通过 | [ ] |
| EDIT-004 | P0 | Tree-sitter 增量解析与高亮，语言可扩展且 grammar 独立 pin | C/C++、Rust、JS/TS、Python、JSON、YAML、TOML、Markdown、Shell、CMake fixture | [x] |
| EDIT-005 | P0 | 大文件降级明确，不在 UI 线程全量解析 | 阈值测试和性能报告 | [ ] |
| EDIT-006 | P1 | 编辑器搜索、替换、跳转行和基础括号匹配 | 交互测试 | [ ] |

## 8. Git Review 与 DesktopChangeTracker

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| GIT-001 | P0 | libgit2 负责 status、branch、diff、stage、unstage、commit；push 错误可诊断 | 仓库 fixture 测试 | [ ] |
| GIT-002 | P0 | DiffModel 统一表示 file/hunk/line、旧新行号、状态和内容 | unit fixture | [x] |
| GIT-003 | P0 | stage/unstage 文件和 hunk | staged/unstaged 混合场景通过 | [x] |
| GIT-004 | P0 | discard 文件/hunk 前校验内容 hash，冲突则拒绝 | 并发外部修改测试 | [x] |
| GIT-005 | P0 | branch 列表/切换不丢未提交修改，错误明确 | dirty checkout fixture | [ ] |
| GIT-006 | P0 | Review 刷新、diff、Git 操作不阻塞 UI 线程 | 线程断言和慢仓库测试 | [ ] |
| CT-001 | P0 | DesktopChangeTracker 在 agent run 前记录 baseline | baseline 包含 Git 状态、文件 hash 和 run id | [x] |
| CT-002 | P0 | preimage 使用内容寻址存入 Desktop `change-snapshots` | 去重、校验、配额测试 | [ ] |
| CT-003 | P0 | run 后生成 ChangeSet 并区分 agent 前已有修改 | 混合修改 fixture | [x] |
| CT-004 | P0 | Accept 只确认该 ChangeSet，不修改文件内容 | 语义测试 | [x] |
| CT-005 | P0 | Reject 只回滚属于该 ChangeSet 的修改并做 hash guard | 并发用户修改不被覆盖 | [x] |
| CT-006 | P0 | ChangeTracker 是 Desktop 私有能力，不新增 daemon 协议 | 依赖/协议审计 | [x] |

## 9. 跨平台 Terminal（不引入完整 Ghostty 应用）

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| TERM-001 | P0 | 只链接固定 revision 的 `libghostty-vt`，不嵌入完整 Ghostty GUI/runtime | 链接、安装包和 headers 审计 | [x] |
| TERM-002 | P0 | Windows ConPTY，macOS/Linux PTY；进程、resize、EOF、退出码和清理正确 | Windows 实测；其他平台待真实平台复验 | Windows [x]；macOS/Linux `BLOCKED-PLATFORM` |
| TERM-003 | P0 | VT 支持普通/alternate screen、scrollback、cursor、SGR、16/256/truecolor | VT fixture | [x] |
| TERM-004 | P0 | UTF-8、CJK、combining、emoji 宽度与选择一致 | fixture 与截图 | [ ] |
| TERM-005 | P0 | 键盘编码、Ctrl/Alt/Shift、功能键、鼠标模式、bracketed paste | protocol fixture | [ ] |
| TERM-006 | P0 | 多行或含控制序列粘贴必须确认 | E2E | [ ] |
| TERM-007 | P0 | OSC 8 只经显式点击和 scheme 白名单打开 | 安全测试 | [x] |
| TERM-008 | P0 | 多标签独立进程、状态、搜索和关闭确认 | E2E | [x] |
| TERM-009 | P0 | shell profile 支持 PowerShell/cmd/WSL、zsh/bash/fish 与自定义可执行文件 | profile resolver 测试；按平台显示可用项 | [ ] |
| TERM-010 | P0 | scrollback、字体和 profile 从 Desktop 本地设置读取，新会话生效 | 重启恢复测试 | [ ] |
| TERM-011 | P0 | 大量输出不阻塞 UI，终端绘制不创建逐字符 DOM | 100MB 输出压力测试及 DOM 计数 | [x]（416.159 ms；64 KiB 最大 0.676 ms；0 DOM） |
| TERM-012 | P1 | 选择、复制、搜索高亮、链接提示和焦点释放完整 | E2E | [ ] |

## 10. Desktop 私有状态、生命周期与 Daemon 边界

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| STATE-001 | P0 | Windows Known Folder、macOS Application Support/Caches、Linux XDG 由平台 API/约定解析 | 路径单元测试与代码审计 | [ ] |
| STATE-002 | P0 | config/state/cache/log/runtime/recovery/change-snapshots 分离 | 目录布局测试 | [x] |
| STATE-003 | P0 | UI 设置写 `settings.json`，导航写 `ui-state/navigation.json` | 保存/重启恢复测试 | [ ] |
| STATE-004 | P0 | UI 保存不修改工作区 `.tokmon/config.yaml` | 前后目录 hash 对比 | [x] |
| STATE-005 | P0 | 模型、权限、会话等已有共享业务状态继续走现有 daemon 能力 | RPC 调用审计 | [x] |
| STATE-006 | P0 | 本地状态原子写、schema version、损坏文件隔离与默认恢复 | 故障注入测试 | [x] |
| STATE-007 | P0 | 多实例锁/冲突策略明确，不互相覆盖 state | 两实例测试 | [x] |
| STATE-008 | P0 | recovery/cache/log 有配额或保留策略 | 配额单元测试 | [ ] |
| LIFE-001 | P0 | Desktop 启动/退出不拥有或强杀非本次启动的 daemon | 生命周期 E2E | [ ] |
| LIFE-002 | P0 | daemon 不可用时 UI 可启动并明确降级，文件/Review/Terminal 仍可用 | 离线 E2E | [x] |

## 11. 线程、状态传递与性能

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| THR-001 | P0 | UI 线程只处理 SDL/RmlUi/Skia、轻量状态应用和输入 | 线程断言 | [ ] |
| THR-002 | P0 | 文件枚举/搜索/读写、Markdown、syntax、Git、diff、snapshot、PTY IO 在 worker | 慢任务注入时 UI heartbeat 不丢失 | [ ] |
| THR-003 | P0 | worker 不直接持有或修改 RmlUi Element | 静态审计与 thread sanitizer 可用时运行 | [ ] |
| THR-004 | P0 | 异步结果带 generation/cancellation，过期结果不覆盖新状态 | race fixture | [ ] |
| PERF-001 | P0 | 100k 文件、100k 行、4k diff、10k 会话不产生线性 DOM | 自动 DOM 计数 | [ ] |
| PERF-002 | P0 | 用户输入到下一帧可见延迟在正常负载下不超过 50ms p95 | Windows Release 性能报告 | [x]（64 样本，p95 7.5409 ms） |
| PERF-003 | P1 | 空闲时不持续满帧重绘；变脏驱动渲染 | GPU/CPU 采样报告 | [x]（稳态单核 2.0584%） |
| PERF-004 | P1 | 冷启动、内存、滚动帧时间记录且无明显回归 | 与旧版同机对比报告 | [ ] |

## 12. 跨平台构建与分发

| ID | 级别 | 验收项 | 通过标准 | 状态 |
|---|---|---|---|---|
| PKG-001 | P0 | Windows Release 从干净构建目录可编译 | 命令与日志 | [x] |
| PKG-002 | P0 | `cmake --install` 后脱离源码/构建树启动 | 安装态 E2E | [x] |
| PKG-003 | P0 | 安装包包含 RML/RCSS、字体、SVG、runtime DLL 和 notices | manifest 对照 | [x] |
| PKG-004 | P0 | 安装包不包含 Debug CRT、完整 Ghostty、Ghostty 开发头或无关 Browser runtime | 文件与依赖审计 | [x] |
| PKG-005 | P0 | `dependency-manifest.json` 记录直接/传递依赖版本、source/revision、许可证 | schema 和文件校验 | [x] |
| PKG-006 | P0 | 同时提供 `THIRD_PARTY_NOTICES.txt` 与 `licenses/` | 安装态检查 | [x] |
| PKG-007 | P0 | HarfBuzz、FreeType、RmlUi、SDL3、Skia、MD4C、Zep、tree-sitter、libgit2、libghostty-vt 等许可完整 | 清单逐项核对 | [x] |
| PKG-008 | P0 | macOS Metal 和 Linux Vulkan 平台分支无 Windows-only API 泄漏 | 当前主机静态/CMake 审计；真实构建标 `BLOCKED-PLATFORM` | 静态 [x]；真实构建 `BLOCKED-PLATFORM` |
| PKG-009 | P0 | macOS arm64/x64、Windows x64、Linux x64/Wayland/X11 真实构建运行 | 当前 Windows 仅 Windows 可勾选，其他为 `BLOCKED-PLATFORM` | Windows x64 [x]；macOS/Linux `BLOCKED-PLATFORM` |

## 13. Agent Browser（本轮明确暂缓）

下列条目全部记为 `DEFERRED-BROWSER`，不启动浏览器、不下载 Browser runtime、不执行网页操作；本轮只要求 Browser 暂缓不会破坏其他模块。

| ID | 验收项 | 状态 |
|---|---|---|
| BROWSER-001 | agent-browser 版本、开源许可证、完整性和签名验证 | DEFERRED-BROWSER |
| BROWSER-002 | 系统 Chrome/Chromium/Edge 发现和用户自定义路径 | DEFERRED-BROWSER |
| BROWSER-003 | 独立 profile、权限、下载和 URL 安全 | DEFERRED-BROWSER |
| BROWSER-004 | snapshot/click/fill/navigation 与 agent 工具编排 | DEFERRED-BROWSER |
| BROWSER-005 | 用户接管、截图、失败恢复和进程清理 | DEFERRED-BROWSER |
| BROWSER-006 | BrowserWorkspace 与 CEF 可选插件路线 | DEFERRED-BROWSER |

## 14. 自动测试矩阵

| ID | 套件 | 必须覆盖 | 状态 |
|---|---|---|---|
| TEST-001 | Core unit | Markdown AST/stream、安全、grapheme、文档、路径、状态、ChangeTracker、Git diff、VT/profile | [x]（CTest 2/2） |
| TEST-002 | Concurrency | generation、cancel、外部文件竞争、Git 慢任务、PTY 并发退出 | [ ] |
| TEST-003 | Visual token | Palette、字体、图标、geometry manifest | [x] |
| TEST-004 | Golden | 第 4 节全部页面和状态，5 DPI × 3 视口的规定样本 | [ ] |
| TEST-005 | UI E2E | 第 4 节除 Browser 外全部交互，使用真实鼠标/键盘/IME | [ ] |
| TEST-006 | Workspace E2E | `E:/cc/AI/tokmon/tokmon-desk-work-test` 文件、编辑、冲突、review、terminal | [ ] |
| TEST-007 | Offline E2E | daemon 不可用时启动和本地能力 | [x] |
| TEST-008 | Install E2E | 安装态、无源码目录依赖、资源/许可证、窗口/退出 | [x]（37/37） |
| TEST-009 | Stress | 100k 文件、100k 行、4k diff、10k 消息、100MB terminal | [ ] |
| TEST-010 | Regression | `ctest -C Release --output-on-failure` 全通过，旧 desktop/daemon 零 diff | [x] |

## 15. 建议执行命令与报告约定

Windows 主机的最终验收至少执行以下等价流程；实际生成器和 vcpkg 路径可按本机配置替换，但不得跳过 Release 和安装态。

```powershell
cmake -S . -B build/windows-msvc-desk-strict-release -DTOKMON_BUILD_DESKTOP=ON -DTOKMON_BUILD_TESTS=ON
cmake --build build/windows-msvc-desk-strict-release --config Release --target tokmon-desk tokmon-desk-tests tokmon-tests
ctest --test-dir build/windows-msvc-desk-strict-release -C Release --output-on-failure
cmake --install build/windows-msvc-desk-strict-release --config Release --prefix build/windows-msvc-desk-strict-release/install-verified
```

所有自动报告统一放入：

```text
build/windows-msvc-desk-strict-release/e2e/
  acceptance-summary.json
  core-tests.log
  visual-token-report.json
  golden-report.json
  ui-e2e-report.json
  workspace-e2e-report.json
  stress-report-final.json
  performance-report.json
  package-audit.json
  screenshots/
```

报告必须包含：Git commit、构建类型、可执行文件 SHA-256、操作系统、GPU 后端、DPI、物理/逻辑分辨率、测试起止时间、通过/失败/暂缓数量、失败原因和证据路径。

## 16. 验收证据登记

在最终验收前逐项回填，不得只写“手工验证通过”。

| 证据 ID | 对应条目 | 命令或文件 | SHA-256/关键结果 | 结论 |
|---|---|---|---|---|
| EV-001 | BASE-* | `apps/tokmon-desk/assets/visual-baseline-manifest.json` | `0456cee36be7646c63def154187fe66f08dc0362151899b2f5b434284b93f920`；65/65 assets | 通过 |
| EV-002 | TEST-001/002 | `build/windows-msvc-desk-strict-release/e2e/core-tests.log` | `3d4c1b1ecab4ed8a6af2bd4e0abd3d75dac3dacc6920a92314fdb26041e5b5ff`；CTest 2/2 | Core 通过；并发清单未全覆盖 |
| EV-003 | VIS-*/TEST-003/004 | `ui-contract-final.json`、`visual-state-gallery-visual-clone-final/` | 主基线 9/9；几何矩阵 15/15；最终状态图 26/26；完整同步数据 0.5% mask 仍缺 | 主工作台通过；发布 golden 未签 |
| EV-004 | UI-*/TEST-005 | `ui-e2e-visual-clone-final.json` | `28111438abf92d546701b87ecea9cdb98211ba07eb8bbdfbfd9b70e5ca99dbb8`；37/37，五 DPI 185/185 | 自动交互通过；真实 IME 仍未签 |
| EV-005 | WS-*/DOC-*/EDIT-* | `build/windows-msvc-desk-strict-release/e2e/workspace-e2e-report.json` | workspace/UI/core 通过项见报告 | 部分通过，未满足全部行 |
| EV-006 | PERF-*/TEST-009 | `visual-acceptance-final.json`、`performance-report.json` | `b9f38c928048c020d670a31222f526724ea1a202097e3e001b39ee01bf28e0cd`；输入 p95 7.6648 ms | 配置压力通过；文件模型为 10k，未达 TEST-009 的 100k |
| EV-007 | PKG-* | `build/windows-msvc-desk-strict-release/e2e/package-audit.json` | 157 files / 20 DLL / 49 licenses / build-install exe hash 相同 | Windows 通过；其他平台阻塞 |
| EV-008 | 全部 Windows 条目 | 最终 `tokmon-desk.exe` 与本节最终报告 | exe `e849510fca482a67504a16052750dd47c74ce19b3ffcb890515b2895c3e7affa` | 自动化通过，发布门槛未全通过 |
| EV-009 | 旧版保护 | `git diff -- apps/tokmon-desktop apps/tokmond` | 0 行 | 通过 |
| EV-010 | CLONE-* | `visual-clone-final.png`、`legacy-vs-tokmon-desk-final.png` | `24cb31b3fe4d0266f727381d3a506864a3ea82d34ee11280f1c999fa78e6dcce`；并排图 `0071c8b8bd8504e5deec7a3560ec8fb54a33754ae4f9c333f27060653d2b907d` | Codex 视觉复核通过；用户签署待定 |

## 17. 最终签署

只有以下条件同时满足时才能写“除 Browser 外，tokmon-desk 已按设计完整实现并通过本轮验收”：

- [ ] 第 2 至 12 节、第 14 节所有 P0/P1 均已通过或按规则明确标记真实平台阻塞。
- [ ] 第 4 节旧版 UI 页面与状态复刻矩阵全部通过。
- [x] Windows Release 已执行的单元、集成、压力、UI E2E 和安装态自动测试全部通过。
- [ ] 人工旧版/新版并排视觉复核已签署。
- [x] `apps/tokmon-desktop` 与 `apps/tokmond` 零改动。
- [x] Browser 只标记为 `DEFERRED-BROWSER`，最终结论明确排除 Browser。
- [x] macOS/Linux 未在真实机器验证的项目明确写为待验收，不伪报跨平台运行通过。

签署记录：

| 角色 | 姓名 | 日期 | 结论 | 备注 |
|---|---|---|---|---|
| 实现/自动验收 | Codex | 2026-08-30 | 主工作台视觉复刻与 Windows 自动化通过，但存在发布阻塞项 | Browser excluded；主视觉见 EV-010 |
| UI 视觉复核 | 待填写 | 待填写 | **未签署** | 必须并排查看旧版与新版全状态 |
| 发布验收 | 待填写 | 2026-08-30 | **不通过** | VIS-005/VIS-011、逐行全状态矩阵、真实 IME/device-lost、macOS/Linux 真机未完成 |
