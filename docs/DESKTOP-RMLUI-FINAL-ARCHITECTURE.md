# `tokmon-desk` 原生桌面版技术重写最终方案

> 状态：Accepted  
> 日期：2026-08-29  
> 范围：在 `apps/tokmon-desk` 新建 RmlUi Desktop、本地工作台、跨平台 Terminal 和 Agent Browser 连接  
> 旧版边界：现有 `apps/tokmon-desktop` 保持原样，不修改、不迁移、不删除  
> 视觉边界：本项目只是技术栈重写，不进行 UI 改版；新版必须与当前旧版具有相同主题、颜色、风格、布局和交互设计  
> 核心边界：`tokmon-daemon` 保持当前能力、协议和职责不变

## 1. 执行摘要

新版桌面应用正式命名为 `tokmon-desk`，采用一套 C++ 原生 UI 栈：

```text
SDL3 + RmlUi + Skia + HarfBuzz + FreeType
```

新版采用旁路重写：产品名、进程名、CMake target、可执行文件和发行包统一使用 `tokmon-desk`，所有新代码、资源和构建入口进入 `apps/tokmon-desk`。现有 Slint 实现 `apps/tokmon-desktop` 只作为行为、视觉和交互回归基线，不在本项目中修改、移动或删除。新版可以读取旧版 UI token、尺寸、字体和资产来建立独立副本，但构建和运行时不得依赖旧目录。

本项目是技术重写，不是产品 UI 重设计。除了产品名称按既定决策显示为 `tokmon-desk`，用户打开新版后应感觉它就是当前旧版：相同的主题颜色、暖色中性视觉风格、字体、图标、间距、圆角、边框、阴影、面板层级、信息架构、控件状态和交互位置。SDL3、RmlUi、Skia、HarfBuzz 和 FreeType 只替换实现机制，不得成为视觉变化的理由。

Desktop 同时承担以下仅属于桌面产品的能力：

- Chat、设置、导航和工作区布局；
- Markdown、代码块、Diff 和流式消息渲染；
- 文件浏览、基础代码编辑、Git 修改审查；
- 用户交互式跨平台 Terminal；
- Agent Browser 的安装、浏览器发现、Profile、预览和用户接管；
- Desktop 自己的状态、缓存、快照和恢复。

`tokmon-daemon` 不因本次 UI 重写增加 Workspace、Editor、Terminal 或 Browser 专用服务。Desktop 继续通过现有 Snow/RPC 消费 Daemon 的会话、Agent、Photon、LightPath 和事件流。Agent Browser 通过当前已有的 Iris/Techor MCP 能力接入 `agent-browser mcp`，不在 Daemon 内实现浏览器控制代码。

### 1.1 详细能力与技术选型

| 能力域 | 具体能力 | 最终技术选型 | 实现位置 | 选择理由 | 明确边界/不采用项 |
|---|---|---|---|---|---|
| 工程目录 | `tokmon-desk` 源码与资源 | `apps/tokmon-desk` | 仓库内独立应用目录 | 与旧版完全隔离，便于并行构建、回归和回滚 | 不在 `apps/tokmon-desktop` 内增量替换 |
| 旧版保护 | Slint Desktop 基线 | 保留 `apps/tokmon-desktop` 原样 | 只读行为参考与回归对象 | 避免重写期间破坏当前可用版本 | 不修改、不移动、不删除，不作为新版源码目录 |
| UI 视觉与交互 | 与冻结旧版保持一致 | 旧版 Slint + `tokmon-theme.slint` + Golden Screenshots 为基线 | 新版 `rml/themes`、components 和 assets 独立复刻 | 技术迁移不改变用户体验和产品识别 | 不重新配色、不更换风格、不重排现有界面、不以“现代化”为由接受差异 |
| 应用语言 | Desktop 主体 | C++20 | `apps/tokmon-desk/src` | 与现有原生工程及 RmlUi、SDL3、Skia 集成直接 | 不引入 Electron 主进程架构 |
| 窗口平台 | 窗口、事件循环、DPI、显示器 | SDL3 | `src/platform` | 单一跨平台窗口与输入抽象 | SDL3 不承担 UI 排版和最终绘制 |
| 输入平台 | 键盘、鼠标、光标、剪贴板、IME | SDL3 + 平台补充桥接 | `src/platform` | 统一基础事件，同时允许平台级候选框修正 | 不让 RmlUi 直接调用 OS API |
| UI 框架 | 布局、样式、DOM、焦点、事件 | RmlUi | `src/ui`、`rml` | C++ 原生、轻量、HTML/CSS 类开发模型 | 不使用 RmlUi GL3 renderer，不依赖 OpenGL |
| UI 渲染桥 | RmlUi geometry/texture/clip 到 Skia | 自研 `RenderInterface_Skia` | `src/render` | 隔离 RmlUi 与 GPU API，统一文本和自定义控件绘制 | RmlUi 类型不得进入业务服务层 |
| 2D 渲染 | 普通 UI、文本、代码、Diff、Terminal | Skia Ganesh 首版 | `src/render` | 跨平台高质量 2D、文本和现代 GPU 后端 | Graphite 暂不作为首版硬依赖 |
| Windows GPU | GPU surface/present | D3D12 | `SkiaDeviceD3D12` | Windows 现代图形 API | 不使用 OpenGL 3 |
| macOS GPU | GPU surface/present | Metal | `SkiaDeviceMetal` | 避免 macOS OpenGL 弃用风险 | 不使用 OpenGL compatibility path |
| Linux GPU | GPU surface/present | Vulkan | `SkiaDeviceVulkan` | X11/Wayland 下统一现代 GPU 路径 | 软件渲染仅作为诊断或降级方案 |
| 帧调度 | dirty-region、动画与外部事件唤醒 | SDL event loop + Desktop frame scheduler | `src/app`、`src/render` | 空闲时阻塞，降低 CPU 和功耗 | 不采用永久 60 FPS 空转 |
| 字体发现 | 系统字体、捆绑字体与 fallback | Tokmon `FontManager` + 平台字体 API | `src/fonts` | 所有视图共享同一字体来源和回退顺序 | 不让各控件单独加载字体 |
| 字体塑形 | CJK、ligature、combining、bidi run | HarfBuzz | `src/fonts` | 成熟、跨平台、塑形一致 | 关于页通常无需单独弹窗声明，但发行包保留许可文本 |
| 字形栅格化 | glyph outline/raster | FreeType | `src/fonts` | 与 HarfBuzz、Skia atlas 组合成熟 | 许可证按选定授权路径收录 |
| 字形缓存 | glyph atlas、run cache、DPI 分桶 | Tokmon 自研缓存 + Skia | `src/fonts` | 控制内存、DPI 和失效策略 | 不缓存无限制文本布局结果 |
| Markdown 解析 | CommonMark/GFM 风格紧凑索引 AST | chmd | `src/markdown` | C++20、UTF-8-first、零运行时依赖 | 不直接把 parser AST 暴露给 UI |
| Markdown 模型 | 稳定节点、source range、增量替换 | Tokmon Markdown AST | `src/markdown` | 解耦解析器、渲染器和流式消息 | 不以 RmlUi DOM 作为业务 AST |
| Markdown 显示 | 普通节点 RML，重内容自定义绘制 | RmlUi + Skia custom elements | `src/markdown`、`src/ui/elements` | 兼顾样式开发与大内容性能 | 大代码块不逐行永久创建 DOM |
| 文件树 | 枚举、过滤、懒加载、虚拟列表 | Desktop `WorkspaceService` + `FileTreeModel` | `src/workspace` | 本地能力无需扩张 Daemon | 不把整棵目录树放入长期 DOM |
| 文件监控 | 外部修改、创建、删除、重命名 | 平台 watcher 封装 | `src/workspace` | 及时检测磁盘与编辑缓冲冲突 | watcher 事件必须合并和去抖 |
| 文档缓冲 | 文本版本、dirty、保存、undo/redo | Zep Core adapter + Tokmon `DocumentStore` | `src/editor` | 快速获得成熟编辑基础并保持可替换性 | Zep 类型不越过 adapter 边界 |
| 编辑器绘制 | caret、selection、行号、可视行 | `CodeSurface` + Skia | `src/editor` | 大文件只布局和绘制可视区域 | 不使用一行一个 RmlUi 节点 |
| 语法解析 | 增量语法树与 highlight spans | Tree-sitter | `src/editor` worker | 多语言、增量、生态成熟 | 每个 grammar 单独固定版本和审计许可 |
| 搜索 | 当前文件与 workspace 搜索 | 内存搜索 + `rg` 可选适配 | `src/workspace`、`src/editor` | 小文件即时，大范围搜索可取消 | UI 线程不递归扫描工作区 |
| Git 读取 | status、tree/index/workdir diff、rename | libgit2 | `src/review` | 结构化模型适合文件/hunk UI | 不用解析本地化的 `git diff` 文本作为主模型 |
| Git 修改 | stage/unstage hunk、index 操作 | libgit2 + hash/version guard | `src/review` | 精确操作并可做冲突检查 | discard 前必须验证目标和用户意图 |
| Git 命令 | commit、push、credential helper、复杂兼容行为 | 系统 Git，argv 调用 | `src/review` worker | 复用用户凭据和 Git 行为 | 不拼接 shell command string |
| 修改审查 | baseline、ChangeSet、接受/拒绝 | DesktopChangeTracker | `src/review` | 保持 Daemon 不变仍可提供本地审查 | Desktop 离线修改不得伪称 Agent 精确归因 |
| Terminal 进程 | Windows pseudoterminal | ConPTY | `src/terminal` | Windows 原生跨进程终端接口 | 不让 Agent 复用用户交互 Terminal |
| Terminal 进程 | macOS/Linux pseudoterminal | POSIX PTY | `src/terminal` | 标准 Unix 终端进程模型 | 生命周期归 Desktop 所有 |
| Terminal VT | ANSI/VT 状态机、grid、scrollback | 仅 `libghostty-vt` | `src/terminal` adapter | 获得高质量 VT 内核而不引入完整应用 | 不集成完整 Ghostty UI/窗口/配置系统 |
| Terminal 显示 | cell grid、selection、cursor、links | `ElementTerminal` + Skia | `src/terminal`、`src/ui/elements` | 批量绘制，高输出吞吐 | 不使用每 cell 一个 DOM 节点 |
| Agent Browser | 自动化执行与 MCP 接口 | 开源 `agent-browser` | Desktop runtime + 现有 Iris/Techor MCP | 可复用现有工具机制，不扩张 Daemon | 基础 Desktop 不捆绑 Chromium |
| 浏览器发现 | 系统 Chrome/Chromium/Brave 定位与版本检查 | DesktopBrowserManager | `src/browser` | 优先复用本机浏览器、减少体积 | 不默认复用用户日常 Profile |
| 浏览器会话 | CDP endpoint、独立 profile、headed/headless | agent-browser + Tokmon Profile | `src/browser`、runtime 数据目录 | 隔离自动化数据和用户数据 | 不能未经确认连接任意调试端口 |
| 浏览器预览 | screenshot/低帧率 preview、用户接管 | Agent-browser adapter + Skia image view | `src/browser` | 首版避免嵌入完整浏览器引擎 | 不承诺首版无缝网页嵌入 |
| 真正网页嵌入 | 可选 Browser Provider | 后期独立 `tokmon-cef-host` | 可选 sidecar/plugin | 仅在需求证明值得时承担体积和安全成本 | CEF ABI 和 Chromium 进程不得进入主 UI 核心 |
| Daemon 通信 | 会话、Agent、Photon、LightPath、事件流 | 现有 Snow/RPC client | `src/integration` | 保持现有协议和服务边界 | 不新增 Desktop 专属 RPC |
| 本地状态 | 布局、打开文件、最近工作区、快照 | Desktop 本地数据库/配置 | 平台 `DeskAppPaths` | 与用户级/项目级 `.tokmon` 分离，不污染工作区 | 不建立第二套模型、Provider 或 LightPath 配置源 |

### 1.2 `tokmon-desk` 命名矩阵

| 对象 | 新版正式名称 | 说明 |
|---|---|---|
| 产品名/界面显示名 | `tokmon-desk` | 设置、关于、窗口标题和发布说明统一使用 |
| 源码目录 | `apps/tokmon-desk` | 新版唯一源码根 |
| CMake target | `tokmon-desk` | 不与旧版 target 混用 |
| Windows 可执行文件 | `tokmon-desk.exe` | 进程名随可执行文件 |
| macOS/Linux 可执行文件 | `tokmon-desk` | bundle 内主程序和 Linux binary 均使用该名称 |
| 安装包/发行产物 | `tokmon-desk` | 平台扩展名按打包系统追加 |
| Desktop 路径解析器 | `DeskAppPaths` | 返回 config/data/state/cache/logs 等平台规范路径，不使用字面目录 `tokmon-desk-data` |
| 日志进程标签 | `tokmon-desk` | 便于与旧版和 `tokmon-daemon` 区分 |
| 旧版源码标识 | `apps/tokmon-desktop` | 仅指冻结的 Slint 旧实现，不作为新版别名 |

## 2. 目标与非目标

### 2.1 目标

1. 在 Windows、macOS、Linux 上使用同一套 UI、文本、编辑器和 Terminal 表现。
2. macOS 不依赖 OpenGL，Windows/macOS/Linux 使用各自现代 GPU API。
3. 基础安装不捆绑 Chromium，也不捆绑完整 Ghostty。
4. 保留当前 Tokmon Desktop 已有的文件树、只读预览、Git Diff、stage、commit、push 等能力，并演进为可编辑、可按 hunk 审查的工作台。
5. Agent Browser 优先使用用户电脑已有的 Chrome/Chromium，并使用 Tokmon 独立 Profile。
6. Desktop 能够独立开发和演进，不扩大 `tokmon-daemon` 的产品 UI 职责。
7. 所有大列表、大文件、Diff 和 Terminal 均采用可视区域渲染，避免 DOM 节点数量与内容规模线性增长。
8. 对第三方依赖固定版本、保存许可证和 notices，并为不稳定 API 设置 Tokmon 自有适配层。
9. 新版只在 `apps/tokmon-desk` 实现，并保持 `apps/tokmon-desktop` 的源码、资源和构建定义不变。
10. 新版完整复刻当前旧版的 UI 主题、颜色、字体、图标、布局、组件外观、信息架构和交互行为。

### 2.2 非目标

首版不实现：

- Electron 或 Web 技术主 UI；
- OpenGL 渲染后端；
- SDL_GPU 作为 Skia 与窗口之间的中间渲染层；
- 完整 IDE，包括 Debugger、重构、多光标和全量 LSP 体验；
- 在 Diff 视图中直接编辑；
- 完整浏览器引擎或 Chromium fork；
- CEF 无缝内嵌；
- 让 Agent 直接操控用户正在使用的 Desktop Terminal；
- UI 重新设计、品牌风格调整、主题换色、字体替换或视觉“现代化”；
- 重排导航、面板、设置页、会话页和审查页的信息架构；
- 借技术迁移改变既有控件尺寸、密度、文案、图标、hover/pressed/selected/focus 状态；
- 修改、搬迁、删除或逐步改造 `apps/tokmon-desktop`；
- 为 Desktop 功能新增 Daemon RPC 或 Daemon 专用服务。

## 3. 关键架构原则

### 3.1 Daemon 零扩张

`tokmon-daemon` 继续保持当前边界：

- Agent、模型、会话、Photon、LightPath；
- 当前工具执行、文件、Git、进程和 PTY 能力；
- 当前 MCP/LSP、Iris、Techor、Styx 等能力；
- 当前 Snow/RPC 和事件投影。

本方案不向 Daemon 添加：

- Desktop WorkspaceService；
- Desktop DocumentService；
- Desktop Git Review Service；
- 用户交互式 Terminal Session；
- `libghostty-vt`；
- Browser Runtime Manager；
- CEF；
- 浏览器帧传输；
- Desktop 状态数据库。

### 3.2 Desktop 是完整的本地工作台

Desktop 直接拥有窗口、UI、文件浏览、编辑、Git 审查、用户 Terminal 和 Browser UI。它可以调用本地文件系统和 Git，但这些行为必须遵守 workspace containment、并发版本检查和用户确认规则。

### 3.3 业务模型与 UI 框架解耦

RmlUi 类型不得进入 Workspace、Document、Diff、Terminal 和 Browser 核心模型。UI 层只负责把不可变快照或增量 patch 投影为 RmlUi DOM、自定义 Element 和 Skia 绘制命令。

### 3.4 大内容使用自定义 Element

以下内容不得按“一个字符/单元格/文件/代码行一个长期 DOM 节点”的方式实现：

- Terminal；
- 代码编辑器；
- 大型 Diff；
- 大型文件树；
- 长会话虚拟列表。

它们应使用扁平模型、可视范围计算和批量 Skia 绘制。

### 3.5 视觉与交互一致性是硬约束

冻结旧版是新版唯一的视觉产品基线。优先级如下：

1. 指定基线 commit 构建出的 `apps/tokmon-desktop` 实际界面；
2. `apps/tokmon-desktop/ui/tokmon-theme.slint` 中的 Palette 和图标映射；
3. 旧版各 Slint component 的尺寸、间距、字体、状态和布局规则；
4. `apps/tokmon-desktop/assets` 中的 MiSans VF、Figma SVG 和其他现有资产；
5. 固定数据 fixture、窗口尺寸、DPI 和平台生成的 Golden Screenshots。

旧目录只读。实施时把必要的 token、字体和资产复制到 `apps/tokmon-desk` 并保留来源清单、hash 和许可证，新版不得在构建或运行时引用 `apps/tokmon-desktop` 的相对路径。

| 视觉/交互维度 | 必须保持的内容 | 验证方式 | 不允许的变化 |
|---|---|---|---|
| 品牌与整体风格 | 当前暖色中性色调、留白、层级和轻量边框语言 | 并排截图和人工设计审查 | 改为深色科技风、系统原生风、Material、Fluent 或其他新风格 |
| 颜色 | `Palette` 的背景、surface、border、hairline、文字、accent、状态色和 Diff 色值 | token 单测要求 RGBA 精确一致；截图复核混合结果 | 调整色相、亮度、透明度或对比关系；以 Skia 色彩管理为由换色 |
| 字体 | 现有 UI 默认使用 MiSans VF；字号、字重、行高、字距和 fallback 语义保持 | 字体资源 hash、glyph bounds、baseline 和截图比较 | 把现有界面字体改为 Inter、系统字体或其他新字体 |
| 图标 | 现有 Figma SVG 的路径、尺寸、颜色和语义 | SVG hash/规范化 path 对比和截图 | 换用另一套 icon library、改变 stroke/fill 风格 |
| 信息架构 | 导航、会话、设置、工作区、右侧面板和 Overlay 的层级与位置 | 逐页面结构清单和交互回放 | 合并、拆分、重命名或移动现有入口 |
| 布局几何 | pane 比例、rail、sidebar、toolbar、卡片、列表、输入区和对话框几何 | 固定 viewport 下 geometry snapshot；目标误差不超过 1 logical px | 为适配 RmlUi 擅自改变宽高、padding、gap、alignment |
| 组件造型 | 圆角、边框、阴影、分隔线、胶囊、按钮、输入框和卡片 | component golden tests | 改变组件密度或视觉层级 |
| 控件状态 | normal、hover、pressed、focus、selected、disabled、loading、success、warning、danger | 状态矩阵截图和事件测试 | 缺失状态、使用框架默认样式或新增突兀效果 |
| 文案 | 现有标题、标签、提示、空状态、按钮和错误文案 | fixture snapshot 和字符串清单 | 借迁移重写产品文案；产品名替换为 `tokmon-desk` 是唯一全局命名例外 |
| 交互 | 点击目标、快捷键、展开/折叠、tab、焦点顺序、滚动和 Overlay 行为 | 录制的行为场景回放 | 改变用户完成现有任务的路径或控件位置 |
| 动效 | 现有 duration、easing、spinner 和过渡语义 | 时间线/关键帧测试与录屏复核 | 添加无基线的新动画或删除有意义的反馈 |
| 窗口 | 初始尺寸、最小尺寸、标题栏、面板恢复和 resize 行为 | 多 DPI 窗口测试 | 因 SDL3 接入改变窗口结构和默认布局 |

允许存在的实现级差异只有：不同渲染器导致的亚像素抗锯齿、平台字体 rasterization 和 GPU 色彩舍入。这些差异不能改变字体度量、换行、控件几何或可感知色彩。Golden 测试应对文字边缘和动画区域设置窄 mask，不能通过大范围提高容差掩盖布局或颜色偏差。

### 3.6 新能力的 UI 延伸规则

Terminal、可编辑 CodeSurface 和 Agent Browser 是旧版未完整覆盖的新能力，因此无法逐像素复刻不存在的页面。它们必须遵守以下规则：

- 使用旧版 Palette、MiSans VF、图标语法、圆角、边框、阴影和间距尺度；
- 复用旧版已有 pane、tab、toolbar、dialog、empty state 和 status pattern；
- 不为了容纳新能力重新设计已有会话、设置、导航或审查页面；
- 新入口优先进入既有扩展位；若必须改变现有信息架构，作为独立 UI 设计提案审批，不计入技术重写；
- Terminal 内容区可以使用经批准的等宽字体，但其 tab、toolbar、边框和外围容器必须保持旧版风格；
- CodeSurface 的代码内容可以使用等宽字体，文件树、tab、按钮和 Diff chrome 必须保持旧版风格；
- Browser preview 的网页内容不受 Tokmon theme 控制，但其容器、权限条、toolbar、加载和错误状态必须保持旧版风格。

### 3.7 视觉基线变更控制

技术重写 PR 不得顺手更新 Golden baseline。任何有意视觉变化必须：

1. 脱离技术迁移单独提出 UI 变更；
2. 列出旧图、新图和设计理由；
3. 明确受影响页面、状态、DPI 和平台；
4. 获得产品/UI 设计批准后再更新 token 和 Golden Screenshots；
5. 不回写或修改 `apps/tokmon-desktop`。

## 4. 系统与进程架构

```text
┌──────────────── tokmon-desk（apps/tokmon-desk）───────────────┐
│                                                                │
│  SDL3                                                          │
│  ├─ Window / Event Loop / DPI                                  │
│  ├─ Keyboard / Mouse / Clipboard                               │
│  └─ IME / Text Input                                           │
│                                                                │
│  RmlUi + RenderInterface_Skia                                  │
│  ├─ Chat / Settings / Navigation                               │
│  ├─ MarkdownView                                               │
│  ├─ FileTree / CodeSurface / DiffReview                        │
│  ├─ ElementTerminal                                            │
│  └─ BrowserWorkspace                                           │
│                                                                │
│  Desktop Services                                              │
│  ├─ WorkspaceService / FileWatcher                             │
│  ├─ DocumentStore / SyntaxService                              │
│  ├─ GitService / DiffService / DesktopChangeTracker            │
│  ├─ DesktopTerminalService                                     │
│  └─ DesktopBrowserManager                                      │
└────────────────────────────┬───────────────────────────────────┘
                             │ 现有 Snow/RPC
┌────────────────────────────▼───────────────────────────────────┐
│ tokmon-daemon：保持当前能力和协议不变                          │
└────────────────────────────┬───────────────────────────────────┘
                             │ 现有 Iris/Techor MCP
                    ┌────────▼─────────┐
                    │ agent-browser mcp │
                    └────────┬─────────┘
                             │ CDP
                    ┌────────▼─────────┐
                    │ Chrome/Chromium   │
                    │ Tokmon Profile    │
                    └───────────────────┘
```

### 4.1 详细责任划分

| 所有者/模块 | 运行位置 | 负责内容 | 主要输入 | 主要输出 | 明确不负责 |
|---|---|---|---|---|---|
| `tokmon-desk` / `apps/tokmon-desk` | 新版桌面主进程与源码根 | 新 UI、Desktop 本地工作台以及所有下列 Desktop 子模块的组装 | SDL 事件、Daemon 事件、本地文件与子进程事件 | 窗口画面、用户命令、本地状态 | 不修改旧版目录，不实现 Agent/模型核心 |
| `apps/tokmon-desktop` | 旧版 Slint Desktop | 维持当前可用实现，作为功能行为和回归基线 | 现有构建与运行输入 | 现有 Slint Desktop | 不接收本次重写代码，不做渐进式内部替换 |
| `DesktopApp` | 新版 Desktop UI 线程 | 生命周期、窗口、顶层导航、命令路由、焦点和 frame scheduling | 用户输入、各服务不可变 snapshot | UI state、服务命令、重绘请求 | 不执行磁盘扫描、Git、解析和阻塞 I/O |
| `SdlPlatform` | 新版 Desktop UI 线程 | Window、event loop、DPI、鼠标键盘、clipboard、IME 和 native handle | OS 事件 | 归一化 Desktop input event | 不负责 DOM、字体塑形或业务状态 |
| `DeskAppPaths` | 新版 Desktop 启动阶段 | 通过平台 API 解析 config/data/state/cache/logs 绝对路径并创建受控子目录 | OS known folders、XDG 环境与 fallback | 不可变 `DeskAppPathsSnapshot` | 不返回 `.tokmon`，不退回 cwd，不决定共享配置位置 |
| `LegacyVisualBaseline` | 新版构建/测试工具链 | 固定旧版 commit、Palette、字体/图标 hash、geometry、fixtures、Golden Screenshots 和交互场景 | 冻结旧版只读源码与可执行结果 | baseline manifest、RCSS token、golden fixtures、差异报告 | 不修改旧目录，不自动接受新基线，不进行视觉设计 |
| `RmlUiHost` | 新版 Desktop UI 线程 | RML/RCSS 文档、DOM 生命周期、普通控件、焦点和事件传播 | ViewModel、输入事件 | 布局结果、UI action、自定义 Element 边界 | 不保存 Workspace/Document/Git 主状态 |
| `RenderInterface_Skia` | 新版 Desktop UI 线程 | 将 RmlUi geometry、texture、clip 和 transform 转换为 Skia 命令 | RmlUi render calls | SkCanvas draw calls | 不包含业务组件逻辑，不直接管理 OS window |
| `SkiaDevice` | UI 线程及平台 GPU 上下文 | GPU device、surface、resize、frame begin/end、present、设备丢失恢复 | 原生窗口句柄、尺寸、dirty frame | SkSurface/SkCanvas 和 present 结果 | 不关心 RmlUi DOM、Markdown 或 Terminal 语义 |
| `FontManager` | UI 线程 + 可控缓存任务 | 字体发现、fallback、HarfBuzz shaping、FreeType face、glyph atlas/cache | UTF-8、字体样式、locale、DPI | positioned glyph runs、glyph resources | 不决定编辑器行布局或 Terminal cell 语义 |
| `MarkdownService` | Worker 解析，UI 投影 | chmd AST、Tokmon AST、source range、流式增量和安全链接模型 | 消息 Markdown、stream delta | versioned AST/snapshot | 不把 RmlUi DOM 当作源数据，不执行链接 |
| `MarkdownView` | 新版 Desktop UI 线程 | AST 到 RML/自定义 Element 的可视投影、选择和复制 | Markdown AST、theme、viewport | DOM patch、Skia draw data、UI actions | 不解析原始 Markdown，不访问磁盘 |
| `WorkspaceService` | Desktop worker | 根目录约束、目录枚举、文件元数据、忽略规则、搜索协调 | workspace root、用户文件命令 | versioned workspace snapshot | 不越过 workspace containment，不充当 Daemon 文件服务 |
| `FileWatcher` | Desktop 平台 worker | 监听文件创建、修改、重命名和删除，合并突发事件 | OS watcher event | 去抖后的 path change batch | 不直接修改 Document 或 UI |
| `DocumentStore` | Desktop worker/state owner | 文档 buffer、版本、dirty、encoding、保存、外部冲突、undo/redo 协调 | open/edit/save 命令、watcher 事件 | immutable document snapshot、save result | 不绘制编辑器，不在无版本检查时覆盖磁盘 |
| `EditorCoreAdapter` | Desktop 编辑器层 | 将 Zep Core 封装为 Tokmon 编辑命令和 buffer 接口 | 文本命令、selection、keymap | edit transaction、caret/selection state | 不暴露 Zep 类型给 Workspace、Review 和 UI 外层 |
| `SyntaxService` | Desktop worker pool | Tree-sitter parser 生命周期、增量 edit、highlight/fold spans | 文档版本和 edit delta | 带 document version 的语法结果 | 过期结果不得覆盖新文档版本 |
| `CodeSurface` | 新版 Desktop UI 线程 | 可视行布局、caret、selection、行号、decorations 和 Skia 批绘制 | document snapshot、syntax spans、viewport | editor pixels、hit-test result、edit intent | 不执行文件 I/O 或 Tree-sitter parse |
| `GitService` | Desktop worker | repository 发现、status、index、Git 命令队列和刷新 | workspace、Git action | repository snapshot、operation result | 不负责 UI DOM，不修改 Daemon |
| `DiffService` | Desktop worker | HEAD/index/worktree/open-buffer diff、rename、hunk 模型 | Git blobs、disk、Document snapshot | versioned DiffModel | 不在 UI 线程计算大 diff |
| `DesktopChangeTracker` | Desktop worker + 本地存储 | Agent 工作前基线、pre-image、ChangeSet、接受/拒绝状态 | 现有 run/turn 事件、watcher、Git 和 buffer hash | Desktop-local ChangeSet | 不提供 Daemon 级审计保证，不声称离线修改的精确归因 |
| `DiffReviewView` | 新版 Desktop UI 线程 | 文件/hunk 虚拟化、unified/split 绘制、审查交互 | DiffModel、repository state | stage/unstage/discard/accept/reject intent | 不绕过 hash guard 直接写文件或 index |
| `DesktopTerminalService` | Desktop worker + PTY I/O threads | 用户 Terminal tab、shell profile、PTY 生命周期、resize、I/O 和退出 | 用户命令、Terminal input、window size | VT byte stream、session state | 不替换 Daemon 现有 Agent PTY，不被 Agent 默认接管 |
| `TerminalEmulator` | Desktop terminal worker | `libghostty-vt` adapter、screen grid、cursor、scrollback、mode | PTY VT bytes、resize/input mode | immutable/delta render snapshot | 不创建进程，不绘制像素，不暴露 Ghostty 内部 ABI |
| `ElementTerminal` | 新版 Desktop UI 线程 | cell 批绘制、selection、copy、links、IME、鼠标协议和输入转发 | TerminalRenderSnapshot、viewport、SDL input | Skia commands、PTY input intent | 不解析 shell 命令，不逐 cell 创建 DOM |
| `DesktopBrowserManager` | Desktop worker + UI coordination | runtime 安装、签名校验、浏览器发现、profile、进程、权限和 session | 用户设置、browser action、runtime manifest | browser session state、preview frames、MCP config | 不进入 Daemon，不默认读取用户日常 Chrome Profile |
| `agent-browser adapter` | Desktop runtime/sidecar 边界 | 启停 agent-browser、协议适配、CDP endpoint、截图和 preview | BrowserManager command、MCP/browser events | normalized browser events/actions | 不把 agent-browser 私有类型泄漏到 UI 核心 |
| 现有 Snow/RPC client | 新版 Desktop integration worker | 使用既有协议订阅会话、Agent、Photon、LightPath 和事件流 | `tokmon-daemon` 现有 RPC | Desktop domain event/snapshot | 不新增 Desktop Workspace/Editor/Terminal/Browser RPC |
| `tokmon-daemon` | 现有独立进程 | 保持当前 Agent、模型、会话、工具、PTY、MCP/LSP、Snow/RPC 能力 | 现有请求与工具配置 | 现有响应和事件 | 不拥有新版 Desktop 的文件编辑、Git UI、用户 Terminal 或 Browser UI |
| 现有 Iris/Techor MCP | Daemon 现有工具链 | 按当前通用 MCP 机制连接 `agent-browser mcp` | Agent tool call、现有 MCP 配置 | 浏览器工具结果 | 不增加浏览器专用 Daemon 服务或协议 |
| 系统 Git | Desktop 启动的外部进程 | commit、push、credential helper 和复杂兼容行为 | argv、cwd、受控环境 | stdout/stderr、exit code、repository change | 不接受拼接 shell 字符串，不在 UI 线程运行 |
| 系统 Chrome/Chromium | Desktop 管理的外部进程 | 页面执行、网络、渲染、CDP target | 独立 Tokmon Profile、URL、CDP command | 页面状态、截图、下载/权限事件 | 不捆绑进基础包，不默认共用用户 Profile |
| `tokmon-cef-host`（未来可选） | Desktop plugin/sidecar 进程 | 真正内嵌浏览器 surface、输入和进程隔离 | Browser Provider protocol | 共享帧、browser events | 不进入首版，不链接进 Daemon，不把 CEF ABI 暴露给主业务层 |

### 4.2 生命周期与故障边界

进程生命周期：

- `tokmon-desk` 退出时，Desktop 文件缓存、编辑器和用户 Terminal 按 Desktop 策略保存/关闭；
- `tokmon-daemon` 生命周期保持现状；
- `agent-browser` 是可选 sidecar，可由 Desktop 启动和回收；
- 浏览器使用独立 Tokmon Profile，不默认接管用户日常 Profile；
- 未来的 `tokmon-cef-host` 也是 Desktop 可选组件，不成为 Daemon 子系统。

## 5. Desktop 目录建议

### 5.1 新旧目录规则

| 路径 | 定位 | 本次允许操作 | 本次禁止操作 | 构建关系 |
|---|---|---|---|---|
| `apps/tokmon-desk` | 新版 RmlUi Desktop 唯一源码根目录 | 新建和修改 C++、RML、RCSS、资源、测试及构建文件 | 反向依赖旧版内部实现 | 产生新版独立 target/package |
| `apps/tokmon-desktop` | 现有 Slint Desktop、回归基线 | 读取行为、运行对照测试 | 修改、移动、删除、格式化、批量重写、植入兼容层 | 保持现有 target/package 不变 |
| 公共库目录 | 新旧应用可能共同依赖的稳定公共能力 | 仅在本身属于公共需求且兼容现有调用方时独立修改 | 为绕过目录隔离而搬出旧版私有代码 | 变更必须单独测试所有既有调用方 |

新版可以依据旧版的外部行为重新实现能力，但不得通过修改旧目录来给新版铺路。若确实发现必须抽取的通用能力，应作为单独设计和变更评审处理，不属于本 UI 重写的默认授权范围。

### 5.2 新版目录结构

```text
apps/tokmon-desk/
├─ src/
│  ├─ app/
│  │  ├─ desktop_app.*
│  │  ├─ desktop_state.*
│  │  └─ command_router.*
│  ├─ platform/
│  │  ├─ sdl_platform.*
│  │  ├─ clipboard.*
│  │  ├─ ime_bridge.*
│  │  ├─ desk_app_paths.*
│  │  └─ native_dialogs.*
│  ├─ render/
│  │  ├─ skia_device.*
│  │  ├─ skia_device_d3d12.*
│  │  ├─ skia_device_metal.*
│  │  ├─ skia_device_vulkan.*
│  │  └─ rml_render_interface_skia.*
│  ├─ fonts/
│  │  ├─ font_engine_harfbuzz.*
│  │  ├─ font_fallback.*
│  │  └─ glyph_cache.*
│  ├─ markdown/
│  │  ├─ markdown_ast.*
│  │  ├─ chmd_adapter.*
│  │  ├─ markdown_stream.*
│  │  └─ markdown_rml_renderer.*
│  ├─ workspace/
│  │  ├─ workspace_service.*
│  │  ├─ file_watcher.*
│  │  ├─ file_tree_model.*
│  │  └─ workspace_search.*
│  ├─ editor/
│  │  ├─ document_store.*
│  │  ├─ zep_adapter.*
│  │  ├─ code_surface.*
│  │  ├─ code_layout_cache.*
│  │  └─ tree_sitter_service.*
│  ├─ review/
│  │  ├─ git_service.*
│  │  ├─ diff_service.*
│  │  ├─ diff_model.*
│  │  └─ desktop_change_tracker.*
│  ├─ terminal/
│  │  ├─ terminal_service.*
│  │  ├─ terminal_emulator_ghostty.*
│  │  ├─ terminal_renderer_skia.*
│  │  ├─ pty_session.*
│  │  ├─ conpty_session.*
│  │  └─ posix_pty_session.*
│  ├─ browser/
│  │  ├─ browser_manager.*
│  │  ├─ browser_discovery.*
│  │  ├─ agent_browser_client.*
│  │  ├─ browser_profile.*
│  │  └─ browser_preview_model.*
│  ├─ integration/
│  │  ├─ snow_rpc_client.*
│  │  ├─ daemon_event_projector.*
│  │  └─ mcp_browser_config.*
│  └─ ui/
│     ├─ view_models/
│     ├─ elements/
│     │  ├─ element_code_surface.*
│     │  ├─ element_diff_review.*
│     │  └─ element_terminal.*
│     └─ controllers/
├─ rml/
│  ├─ documents/
│  ├─ components/
│  └─ themes/
│     ├─ legacy-palette.rcss
│     ├─ legacy-components.rcss
│     └─ legacy-metrics.rcss
├─ assets/
│  ├─ fonts/
│  ├─ icons/
│  ├─ licenses/
│  └─ visual-baseline-manifest.json
├─ tests/
│  ├─ unit/
│  ├─ integration/
│  ├─ fixtures/
│  ├─ golden/
│  │  └─ legacy-ui/
│  └─ visual/
└─ CMakeLists.txt
```

`apps/tokmon-desk` 是确定目录，不再随构建组织调整。其内部子目录可以在实施中细化，但责任边界应保持。

## 6. SDL3、RmlUi 与 Skia

### 6.1 渲染链路

```text
SDL3 Window
    ↓
RmlUi Layout / DOM / Events
    ↓
RenderInterface_Skia
    ↓
SkSurface / SkCanvas
    ↓
D3D12 / Metal / Vulkan
```

SDL3 负责：

- 窗口创建和事件循环；
- 鼠标、键盘、触摸和光标；
- DPI、显示器变化；
- 剪贴板；
- IME/TextInput；
- 原生句柄获取和系统对话框桥接。

RmlUi 负责：

- 布局、RCSS、DOM 和事件传播；
- 普通控件、弹窗、菜单、工具栏；
- UI 文档和主题；
- 自定义 Element 的布局边界与生命周期。

Skia 负责：

- RmlUi geometry、纹理、clip、transform、layer、filter；
- Markdown 代码块、CodeSurface、Diff 和 Terminal；
- 图片、阴影、渐变和矢量绘制；
- GPU surface 和 present。

### 6.2 Skia 后端封装

业务代码只能依赖：

```cpp
class SkiaDevice {
public:
    virtual SkSurface* surface() = 0;
    virtual SkCanvas* begin_frame() = 0;
    virtual void end_frame() = 0;
    virtual void resize(int physical_width, int physical_height) = 0;
};
```

首版使用稳定的 Ganesh 路径，平台实现分别为 D3D12、Metal 和 Vulkan。Graphite 只在成熟度、驱动覆盖和功能验证满足要求后替换，不能泄露到上层接口。

### 6.3 重绘策略

Desktop 不持续空闲渲染。以下事件设置 dirty：

- SDL 输入；
- RmlUi 状态变化；
- 动画 tick；
- Daemon 事件；
- 文件 watcher；
- 编辑器 caret/selection；
- Terminal 输出；
- Browser preview frame。

没有 dirty、动画和外部帧时阻塞等待事件。

## 7. 字体、中文与 IME

统一文本链路：

```text
UTF-8
  → Font fallback
  → HarfBuzz shaping
  → FreeType glyph rasterization
  → Glyph atlas / positioned glyph runs
  → Skia
```

原则：

1. 测量和绘制必须使用同一 shaping 结果。
2. RmlUi、Markdown、编辑器和 Terminal 共享字体资源管理器。
3. Terminal 保持自己的 cell-grid 语义，但字形仍使用相同 HarfBuzz/FreeType 资源。
4. DPI 变化发生在布局和 glyph atlas 选择之前，不能放大低分辨率 atlas。
5. SDL3 IME composition 必须传入当前拥有文本焦点的 RmlUi 输入框、CodeEditor 或 Terminal。

字体策略以旧版视觉一致性为先：

- 现有 UI、Chat、设置、导航、文件树、审查 chrome 和既有代码预览继续使用旧版同版本 `MiSans VF`；
- 字号、字重、行高、字距、baseline 和 fallback 顺序从冻结旧版提取，不根据 RmlUi/Skia 默认值重新设计；
- 新增 Terminal 和完整代码编辑内容区确需等宽字体时，必须单独确定并通过视觉评审；不得把 JetBrains Mono 或系统 monospace 直接设为全局默认；
- Noto Sans CJK、Noto Emoji 等只可以作为缺字 fallback，不能改变已有字符原本选择的 MiSans 字形；
- 测量和绘制必须共享同一 HarfBuzz shaping 结果，避免因迁移导致换行、按钮宽度和基线变化。

每个字体文件的实际许可证和 notice 必须单独进入发行包；不得假设所有字体均为 OFL。MiSans VF 沿用旧版相同、未经修改的字体文件和官方许可条件。

## 8. Markdown

### 8.1 解析链路

```text
Markdown UTF-8
    ↓
chmd indexed AST
    ↓
Tokmon Markdown AST
    ↓
RmlUi DOM + CodeSurface/Diff custom elements
```

chmd 产生紧凑索引 AST，Desktop 将其映射为带稳定 ID、source range 和安全链接语义的自有 arena AST。

核心节点至少包括：

- Document、Paragraph、Heading、BlockQuote；
- OrderedList、BulletList、ListItem、TaskItem；
- Table、TableRow、TableCell；
- CodeBlock、InlineCode；
- Text、SoftBreak、HardBreak；
- Emphasis、Strong、Strike；
- Link、Image；
- FileReference、DiffBlock、Callout、ToolCall。

每个节点包含稳定 `NodeId`、UTF-8 byte source range、kind、data、parent 和 children。

### 8.2 流式消息

- 每条消息拥有独立 `MarkdownDocument`；
- 已结束的顶层 block 保持稳定；
- 只重解析尾部仍可能变化的 block；
- 流结束后进行一次完整校验重解析；
- UI 接收 block patch，不替换整条消息 DOM。

### 8.3 安全

- 默认禁用 raw HTML；
- 不将模型输出传给 `SetInnerRML`；
- 普通文本创建 TextNode；
- URL scheme 使用白名单；
- 远程图片默认关闭或要求授权；
- 限制输入长度、AST 节点数和嵌套深度；
- Code block 永远按文本渲染。

## 9. 文件浏览与基础编辑

### 9.1 WorkspaceService

Desktop `WorkspaceService` 负责：

- workspace 根路径规范化；
- 路径 containment 和 symlink 检查；
- 异步目录枚举；
- `.gitignore` 和产品排除规则；
- 文件创建、重命名、删除；
- 文件 watcher；
- 文件类型和图标；
- fuzzy 文件搜索。

文件树使用扁平 `FlatTreeModel`，展开文件夹时异步加载，`ElementFileTree` 只绘制可见行。当前固定深度、固定条目数和文件大小限制迁移为可配置保护策略，不作为功能上限。

### 9.2 DocumentStore

每个打开文档记录：

```cpp
struct DocumentSnapshot {
    DocumentId id;
    std::filesystem::path relative_path;
    std::string utf8_text;
    TextEncoding encoding;
    LineEnding line_ending;
    ContentHash disk_hash;
    std::uint64_t version;
    std::uint64_t saved_version;
    bool read_only;
};
```

保存必须满足：

- 检查 workspace containment；
- 比较加载时 `disk_hash` 与当前磁盘内容；
- 外部已修改时显示 reload/compare/overwrite 选择；
- 默认原子写入；
- 保留合理的换行和编码信息；
- 文件权限和 rename 行为按平台处理。

### 9.3 CodeSurface

同一个 `CodeSurface` 支持：

```cpp
enum class CodeSurfaceMode {
    Preview,
    Edit,
    UnifiedDiff,
    SplitDiff,
};
```

共享能力：

- 可视行计算和虚拟滚动；
- 行号栏；
- 字体 shaping 和 glyph run cache；
- selection、搜索匹配和当前行；
- syntax spans；
- 水平滚动；
- 大文件降级模式。

### 9.4 Zep 适配

首版 vendor 固定版本 Zep Core，使用其 buffer、标准编辑命令、selection、undo/redo 和 keymap。不使用 Qt/ImGui 后端，新增 `ZepDisplaySkia` 和 SDL3 输入适配。

Zep 必须封装在 Tokmon 接口后，不允许 UI 其他模块直接依赖 Zep 类型。技术验证必须覆盖：

- 中文 IME composition；
- grapheme/caret；
- undo/redo；
- 剪贴板；
- 高 DPI；
- 10 万行文件；
- 大量连续编辑；
- Windows/macOS/Linux 一致快捷键。

若验证不通过，替换 DocumentBuffer/EditorCore，不改变 `CodeSurface` 与上层 UI。

### 9.5 Tree-sitter

Tree-sitter 在工作线程中增量解析，返回 versioned highlight spans。过期结果不得覆盖新版本文档。首批语言建议：

- C/C++；
- Rust；
- JavaScript/TypeScript/TSX；
- Python；
- JSON/YAML/TOML；
- Markdown；
- Shell；
- CMake。

每个 grammar 独立固定版本并审计许可证。

## 10. Git 与修改审查

### 10.1 GitService

Desktop 保留当前本地 Git UI 归属。职责拆分为：

- libgit2：status、tree/index/workdir diff、rename detection、patch、index 和 hunk 操作；
- 系统 Git：commit、push、credential helper、复杂 checkout/remote 行为；
- 所有进程调用使用 argv，不拼接 shell command string；
- Git 操作在工作线程执行；
- 操作前后刷新 repository/index 状态。

### 10.2 Diff 模型

DiffService 支持以下 source：

```text
Git HEAD
Git Index
Working Tree
Open Document Buffer
Desktop Agent Baseline
```

支持：

- unified/split；
- 按文件和按 hunk 展开；
- stage/unstage 文件；
- stage/unstage hunk；
- discard 文件/hunk；
- 点击行打开对应文件；
- 二进制文件和大文件提示；
- rename/create/delete；
- commit/push。

Diff 第一版只读，不在 Diff cell 中直接编辑。

### 10.3 DesktopChangeTracker

由于 Daemon 不增加 AgentChangeLedger，Desktop 在现有 run/turn/session 事件边界上建立本地基线：

```text
开始一次 Agent 工作
    ↓
记录 Git HEAD、index、status
    ↓
保存已有 dirty/untracked 文件的 pre-image
    ↓
监听本地文件 watcher 和现有 fs.* 事件
    ↓
结束时比较 baseline 与磁盘/打开 buffer
    ↓
生成 Desktop ChangeSet
```

规则：

- 原本干净的 tracked 文件可引用 Git blob 作为 pre-image；
- 原本 dirty 的 tracked 文件必须保存 Desktop 本地内容快照；
- 原本存在的 untracked 文件必须保存快照；
- 后续创建文件的 baseline 为 absent；
- 快照存放于 Desktop 数据目录，不写入 workspace；
- 快照使用内容寻址和大小上限；
- Desktop 未运行期间发生的修改只能标记为“工作区修改”，不能声称精确归因于某次 Agent run。

“接受 Agent 修改”和“Git stage”是两个动作：

- 接受：Desktop 标记该 ChangeSet 已审阅；
- 拒绝：检查当前 hash 后应用反向 patch；
- Stage：写入 Git index；
- Commit：调用 Git 创建提交。

若当前内容与 ChangeSet after-hash 不一致，拒绝操作进入冲突比较，不静默覆盖。

## 11. 跨平台 Terminal

### 11.1 边界

用户可见的交互式 Terminal 完全属于 `tokmon-desk`，不进入 Daemon。Daemon 当前已有的 Agent 命令/PTY 能力保持原样，两者互不替代。

Desktop 退出时，首版用户 Terminal 会话正常关闭。若未来要求 UI 重启后继续运行，可以增加 Desktop 所属的 `tokmon-terminal-host` sidecar，但仍不修改 Daemon。

### 11.2 仅使用 libghostty-vt

不集成完整 Ghostty。只固定并构建 `libghostty-vt`：

```text
Shell process
    ↓
ConPTY / POSIX PTY
    ↓ UTF-8 + VT bytes
libghostty-vt
    ↓ render state
TerminalRenderSnapshot
    ↓
ElementTerminal + Skia
```

`libghostty-vt` 负责：

- ANSI/VT parser；
- screen grid 和 cursor；
- scrollback；
- standard/alternate screen；
- 24-bit/256 color；
- Unicode 和 grapheme；
- resize/reflow；
- keyboard/mouse encoding；
- OSC、SGR、bracketed paste 等终端协议状态。

Tokmon 负责：

- PTY 和 child process；
- shell profile；
- 字体、glyph shaping 和绘制；
- selection UI、clipboard 和搜索；
- 输入、IME、焦点和快捷键；
- 标签页、分屏和关闭确认。

### 11.3 平台 PTY

Windows：

- `CreatePseudoConsole`；
- `ResizePseudoConsole`；
- `STARTUPINFOEX`；
- `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`；
- UTF-8 pipe；
- 支持 PowerShell、cmd 和 WSL profile。

macOS/Linux：

- `posix_openpt`/`openpty`，按平台选取稳定实现；
- `grantpt`、`unlockpt`；
- `setsid`、controlling terminal；
- `termios`；
- `TIOCSWINSZ`；
- 默认 `$SHELL`，支持 zsh、bash、fish。

统一接口：

```cpp
class PtySession {
public:
    virtual Result<void> start(const TerminalLaunchOptions&) = 0;
    virtual Result<void> write(std::span<const std::byte>) = 0;
    virtual Result<void> resize(int columns, int rows) = 0;
    virtual Result<void> signal(TerminalSignal) = 0;
    virtual void close() = 0;
};
```

### 11.4 线程和性能

- PTY I/O 不在 UI 线程；
- 原始字节按小批次送入 `libghostty-vt`；
- Terminal model 生成不可变 frame delta/snapshot；
- UI 每帧最多消费一次合并更新；
- `ElementTerminal` 批量绘制背景、glyph、underline、cursor 和 selection；
- 不为 cell 创建 DOM；
- scrollback 有可配置上限和内存统计。

### 11.5 libghostty-vt 集成策略

- 使用 Ghostty 的 `-Demit-lib-vt` 构建目标；
- 静态或私有动态链接，不安装完整 Ghostty；
- 固定 commit 和 Zig 工具链版本；
- 通过 `TerminalEmulator` 适配层隔离不稳定 C API；
- release 构建启用必要优化；
- 用户运行时不要求安装 Zig 或 Ghostty。

## 12. Agent Browser

### 12.1 总体方案

Agent Browser 使用开源 `agent-browser` sidecar，通过 CDP 控制外部 Chrome/Chromium：

```text
DesktopBrowserManager
    ├─ browser discovery
    ├─ Tokmon profile
    ├─ preview / takeover
    └─ agent-browser client
             ↓ CDP
       Chrome/Chromium
```

它不是内嵌浏览器引擎。首版浏览器窗口为独立窗口，RmlUi 工作区显示 URL、标题、截图/preview stream 和 Agent 操作轨迹。

### 12.2 浏览器发现顺序

1. 用户明确设置的 executable；
2. 系统 Chrome；
3. 系统 Chromium；
4. Brave 或其他明确兼容 Chromium；
5. Playwright/Puppeteer 已安装浏览器；
6. 都不可用时提示按需下载 Chrome for Testing。

Desktop 使用 `--executable-path` 或对应配置传入浏览器路径。

### 12.3 Profile 策略

默认：

```text
系统浏览器 executable
+
Tokmon 独立 profile directory
```

不默认使用用户日常 Chrome Profile。独立 Profile 保存于 Desktop 数据目录，具备独立 cookies、local storage、cache 和 permissions。

连接用户已经运行的浏览器仅作为显式操作：

- 用户主动开启“连接现有浏览器”；
- 使用 `--auto-connect` 或明确 CDP endpoint；
- UI 持续显示“Agent 已连接此浏览器”；
- 退出接管后断开；
- 提示 CDP 等同于高权限浏览器控制。

### 12.4 与现有 Daemon MCP 的连接

Daemon 当前已有 Iris MCP client 和 Techor 外部工具目录。本方案只配置现有机制连接：

```text
agent-browser mcp
```

不修改 Daemon 浏览器代码。Desktop 与 MCP adapter 使用相同的：

- `session`；
- `namespace`；
- browser executable；
- profile path；
- headed/headless 设置；
- 安全策略。

逻辑链路：

```text
Tokmon Agent
    ↓
current Techor
    ↓
current Iris MCP
    ↓
agent-browser mcp
    ↓
agent-browser daemon
    ↓
Chrome/Chromium
```

Desktop 负责 runtime 安装、浏览器发现、Profile 和 UI；Daemon 只把该进程视为普通外部 MCP 工具。

### 12.5 BrowserWorkspace

首版提供：

- 当前 URL、标题、favicon；
- screenshot 或 WebSocket preview stream；
- accessibility snapshot 摘要；
- Agent 当前动作和历史轨迹；
- pause/resume/stop；
- 在外部浏览器接管；
- 下载、上传和权限确认；
- 打开外部 DevTools（开发模式）。

### 12.6 安全

- 默认禁止 `file://`；
- 不向远程网页暴露 Tokmon RPC；
- 独立 Profile；
- CDP endpoint 只绑定 loopback；
- 外部连接使用短生命周期凭据；
- 上传、下载、摄像头、麦克风、定位和通知需要策略或确认；
- 密码、支付和高风险提交进入用户接管；
- 浏览器进程崩溃不影响 Desktop 和 Daemon；
- session state 和认证导出视为敏感数据，不进入 workspace 和 Git。

### 12.7 CEF 后续路线

首版不集成 CEF。只有用户明确需要网页作为 RmlUi 内部可组合表面时，才新增：

```text
BrowserProvider
├─ AgentBrowserProvider
├─ CefEmbeddedProvider
└─ RemoteBrowserProvider
```

CEF 必须运行于 Desktop 可选 `tokmon-cef-host` 进程；先使用共享内存帧，必要时升级为 D3D shared texture、IOSurface、DMA-BUF。CEF ABI 不得暴露给 Desktop 其他模块，更不能进入 Daemon。

## 13. UI 布局

宽屏默认布局：

```text
┌──────────────┬────────────────────────────────┬──────────────┐
│ File Explorer│ Workspace Tabs                 │ Agent / Chat │
│              │                                │              │
│ src/         │ Editor / Diff / Terminal /     │ Messages     │
│ tests/       │ Browser Preview                │ Tool Calls   │
│ README.md    │                                │ Approvals    │
└──────────────┴────────────────────────────────┴──────────────┘
```

要求：

- 三栏均可调整宽度；
- 窄窗口可折叠 File Explorer 和 Agent 面板；
- Editor/Diff/Terminal 获得主要面积；
- 文件单击临时预览，双击固定 tab；
- Editor、Diff、Terminal、Browser 使用统一 workspace tab 模型；
- Chat 和代码工作区之间支持文件/行号引用；
- 所有弹窗和菜单由 RmlUi 绘制，避免原生子窗口 z-order 问题。

## 14. 线程模型

### UI 线程

- SDL event loop；
- RmlUi update/layout；
- Skia frame；
- 轻量状态提交；
- 不执行 Git、目录遍历、Tree-sitter parse、PTY read 或浏览器阻塞调用。

### Worker 执行器

- 文件 I/O 和 workspace enumeration；
- Git/libgit2；
- Tree-sitter；
- Markdown 大文档解析；
- 图片解码；
- Terminal PTY I/O；
- browser process/CLI 调用。

### 状态传递

- worker 结果包含 generation/version；
- UI 只接受仍匹配当前 workspace/document/session 的结果；
- 取消旧任务；
- UI 模型使用不可变 snapshot 或小型 patch；
- Desktop 切换 workspace 时旧 generation 的结果全部丢弃。

## 15. 数据与状态目录

`tokmon-desk` 不创建名为 `tokmon-desk-data` 的实际根目录，也不把 Desktop 私有状态写入用户级或项目级 `.tokmon`。统一由 `DeskAppPaths` 在启动时解析 config、data、state、cache 和 logs 逻辑路径。

### 15.1 `.tokmon` 与 Desktop 本地数据的职责边界

| 数据类型 | 权威位置 | 所有者 | `tokmon-desk` 的访问方式 | 禁止行为 |
|---|---|---|---|---|
| 用户级 Tokmon 配置 | `<user-home>/.tokmon/` | Tokmon runtime/Daemon | 使用现有 Snow/RPC 或既有配置机制 | 不复制成 Desktop 私有配置源 |
| 项目级 Tokmon 配置 | `<workspace>/.tokmon/` | 当前 workspace + Daemon | 使用现有 RPC 读取/更新 `config.yaml`、LightPath 等 | 不保存 UI cache、Terminal history 或 browser profile |
| 模型、Provider、LightPath | 用户级/项目级 `.tokmon` | `tokmon-daemon` | 通过现有 Daemon API 管理 | 不在 `DeskAppPaths` 下维护第二份权威配置 |
| 窗口、布局、主题和最近工作区 | `<desk-config-root>` / `<desk-state-root>` | `tokmon-desk` | Desktop 直接原子读写 | 不写入 workspace `.tokmon` |
| 打开文档恢复和未保存缓冲恢复元数据 | `<desk-state-root>` | `tokmon-desk` | 版本化、本地原子保存 | 不把敏感文件全文无限期保留 |
| DesktopChangeTracker pre-image | `<desk-state-root>/change-snapshots` | `tokmon-desk` | 内容寻址、配额、生命周期清理 | 不写入 workspace，不声称是 Daemon 审计记录 |
| Browser runtime 和 Tokmon Profile | `<desk-data-root>/browser` | `tokmon-desk` | DesktopBrowserManager 独占管理 | 不放入 `.tokmon`，不默认复用日常 Chrome Profile |
| Terminal 本地状态 | `<desk-state-root>/terminal` | `tokmon-desk` | 按用户设置保存有限历史/恢复信息 | 不保存密码，不与 Daemon PTY 状态混用 |
| 可重建缓存 | `<desk-cache-root>` | `tokmon-desk` | 可随时删除和重新生成 | 不作为唯一事实来源 |
| 日志 | `<desk-logs-root>` | `tokmon-desk` | 滚动、配额、脱敏 | 不记录密码、cookies、secret、完整 auth headers |
| 密钥和长期凭据 | OS Credential Manager/Keychain/Secret Service | OS 安全存储 | 使用 opaque key/reference | 不以明文写入 `.tokmon` 或 Desktop 普通文件 |

结论：`.tokmon` 是 Tokmon 的共享运行时/项目配置域；`DeskAppPaths` 是 `tokmon-desk` 的本机 UI 与工作台状态域。两者不会使用同一实际目录，也不能互为备份或镜像。

### 15.2 各平台路径解析

| 逻辑路径 | Windows | macOS | Linux |
|---|---|---|---|
| `<desk-config-root>` | `%LOCALAPPDATA%\Tokmon\tokmon-desk\config` | `~/Library/Application Support/Tokmon/tokmon-desk/config` | `${XDG_CONFIG_HOME:-$HOME/.config}/tokmon-desk` |
| `<desk-data-root>` | `%LOCALAPPDATA%\Tokmon\tokmon-desk\data` | `~/Library/Application Support/Tokmon/tokmon-desk/data` | `${XDG_DATA_HOME:-$HOME/.local/share}/tokmon-desk` |
| `<desk-state-root>` | `%LOCALAPPDATA%\Tokmon\tokmon-desk\state` | `~/Library/Application Support/Tokmon/tokmon-desk/state` | `${XDG_STATE_HOME:-$HOME/.local/state}/tokmon-desk` |
| `<desk-cache-root>` | `%LOCALAPPDATA%\Tokmon\tokmon-desk\cache` | `~/Library/Caches/Tokmon/tokmon-desk` | `${XDG_CACHE_HOME:-$HOME/.cache}/tokmon-desk` |
| `<desk-logs-root>` | `%LOCALAPPDATA%\Tokmon\tokmon-desk\logs` | `~/Library/Logs/Tokmon/tokmon-desk` | `${XDG_STATE_HOME:-$HOME/.local/state}/tokmon-desk/logs` |

Windows 首版全部使用 `%LOCALAPPDATA%`，不引入 Roaming 状态。若未来需要跨设备同步少量 Desktop 偏好，应单独设计同步 schema，不能直接同步 browser profile、Terminal history、缓存或 change snapshots。

路径解析必须使用平台 API 和已解析的绝对路径，不能通过字符串拼接假设环境变量一定存在。Linux 缺少 XDG 环境变量时使用表中 fallback；Windows 和 macOS 查询失败时应明确报错，不退回当前工作目录或用户主目录下的自定义文件夹。

### 15.3 逻辑目录布局

```text
<desk-config-root>/
└─ settings.json

<desk-data-root>/
└─ browser/
   ├─ runtime/
   ├─ profiles/
   └─ state/

<desk-state-root>/
├─ ui-state/
├─ document-recovery/
├─ change-snapshots/
└─ terminal/
   └─ optional-history/

<desk-cache-root>/
├─ glyphs/
├─ syntax/
└─ thumbnails/

<desk-logs-root>/
└─ tokmon-desk.log
```

### 15.4 强制约束

- 不创建 `~/tokmon-desk-data`、`~/.tokmon/tokmon-desk-data` 或 `<workspace>/.tokmon/tokmon-desk-data`；
- 不在工作区根目录创建 Desktop 私有隐藏目录；
- browser state、Terminal state 和 change snapshots 不写入 workspace；
- Desktop 设置只能包含 UI/本机工作台偏好，共享模型与运行时配置继续以 `.tokmon` 为权威来源；
- 缓存必须可删除并重建，并实施独立磁盘配额；
- snapshot、document recovery、browser profile 和日志分别实施保留周期；
- 敏感浏览器状态不得以明文进入普通日志；
- 日志不得记录密码、cookies、secret 值或完整 auth headers；
- 所有写入使用原子替换或事务策略，崩溃后不得留下被误判为有效状态的半写文件。

## 16. 依赖、版本和许可证

所有依赖必须固定 tag/commit，构建时不使用浮动 `main`。发行包提供：

```text
THIRD_PARTY_NOTICES.txt
licenses/
dependency-manifest.json
```

初步许可证清单：

| 组件 | 许可证/注意事项 |
|---|---|
| SDL3 | zlib |
| RmlUi | MIT |
| Skia | BSD-style，连同第三方 notices |
| HarfBuzz | Old MIT，保留版权和免责声明 |
| FreeType | FTL/GPL 双许可，发行时选择并保留对应文本 |
| chmd | MIT |
| Zep | MIT |
| Tree-sitter | 核心 MIT；每个 grammar 单独审计 |
| libgit2 | GPLv2 + Linking Exception；修改 libgit2 时遵守其条款 |
| Ghostty/libghostty-vt | MIT |
| agent-browser | Apache-2.0 |
| 字体 | 每个字体单独审计；MiSans VF 沿用旧版官方许可和 notice，不将其误标为 OFL |
| Chrome for Testing/Chromium | 不属于 agent-browser Apache-2.0，单独审计下载与再分发条款 |
| CEF（未来） | CEF、Chromium 和第三方 notices 独立处理 |

首版优先让用户使用系统浏览器；若需要 Chrome for Testing，默认由可选 runtime 安装器按需下载，避免未经审计直接打入基础包。

## 17. 构建与分发

### 17.1 基础 `tokmon-desk` 包

新版源码根固定为 `apps/tokmon-desk`。CMake target、生成的可执行文件、进程名、安装包标识和用户可见产品名统一为 `tokmon-desk`，正式发行后也不改回旧名称。旧版 target/package 保持现状，两者在迁移期不得发生产物路径或安装标识冲突。

```text
tokmon-desk
SDL3
RmlUi
Skia
HarfBuzz / FreeType
chmd
Zep
Tree-sitter core + selected grammars
libgit2
libghostty-vt
字体、图标、RML/RCSS 和 notices
```

不包含：

- 完整 Ghostty；
- Chrome/Chromium；
- CEF；
- Electron；
- Node/Python runtime（agent-browser 原生 daemon 运行不需要它们）。

### 17.2 可选 Browser Runtime

```text
browser-runtime/
├─ manifest.json
├─ agent-browser
├─ signature
└─ optional downloaded browser metadata
```

`manifest.json` 至少包含：

```json
{
  "id": "tokmon.browser.agent-browser",
  "plugin_version": "pinned-version",
  "protocol_version": 1,
  "platform": "platform-arch",
  "capabilities": [
    "cdp",
    "accessibility_snapshot",
    "screenshots",
    "preview_stream",
    "user_takeover"
  ]
}
```

Runtime 包和更新元数据必须签名并校验 hash。

## 18. 从 Slint 迁移

迁移采用 `apps/tokmon-desk` 内的垂直切片和双实现回归。这里的“迁移”是按原有行为和原有视觉重新实现，不是修改旧版源码或重新设计 UI：所有 Phase 的代码变更都进入 `apps/tokmon-desk` 或经独立评审的公共构建/公共库位置，`apps/tokmon-desktop` 全程保持原样。每个 Phase 同时有功能验收和视觉一致性验收，任意一项未通过都不能进入下一阶段。

### Phase 0A：冻结视觉与交互基线

- 固定旧版基线 commit、构建参数、字体文件 hash 和资产 hash；
- 从 `tokmon-theme.slint` 提取全部 Palette token，不进行调色；
- 建立字号、字重、行高、spacing、radius、border、shadow 和 pane geometry 清单；
- 建立图标名称到现有 Figma SVG 的映射和 hash 清单；
- 为主要页面、Overlay 和控件状态建立固定数据 fixtures；
- 在约定窗口尺寸、DPI 和平台生成 Golden Screenshots；
- 把结果写入 `apps/tokmon-desk/assets/visual-baseline-manifest.json` 和 `tests/golden/legacy-ui`；
- 记录关键交互路径、快捷键、焦点顺序和滚动行为。

Phase 0A 只读取旧版目录，产物全部写入新版目录。没有冻结基线之前，不开始 RML/RCSS 页面复刻。

### Phase 0：技术门槛验证

交付最小原型：

- SDL3 Window；
- RmlUi 文档；
- Skia D3D12/Metal/Vulkan surface；
- HarfBuzz 中文；
- 一个 Markdown 消息；
- 一个 Zep 编辑器；
- 一个 libghostty-vt Terminal；
- 一个 agent-browser 系统 Chrome 会话。
- 一组复刻旧版 Palette、MiSans VF、按钮、输入框、侧栏和对话框的 RmlUi/Skia visual parity 样例。

只有全部通过才进入迁移。

### Phase 1：Desktop Shell

- 按旧版逐像素复刻窗口、标题栏、导航、侧栏、主题和基础组件；
- 复刻旧版 Palette、MiSans VF、Figma 图标、spacing、radius、border 和 shadow；
- 现有 Snow/RPC 接入；
- Settings、Dialog、Clipboard、IME；
- idle redraw；
- Shell、Settings 和 Dialog Golden tests 通过。

### Phase 2：Chat 与 Markdown

- 消息虚拟列表；
- chmd AST 适配；
- 流式增量；
- code block 和文件引用；
- 复制和选择；
- Chat、空状态、流式状态、tool call 和 Markdown Golden tests 通过。

### Phase 3：CodeWorkspace

- 文件树；
- 只读预览；
- Zep 编辑器；
- Tree-sitter；
- 保存、冲突和外部 watcher；
- 快速打开和 workspace 搜索；
- 现有文件树、tab 和代码预览保持原布局与风格。

### Phase 4：Review

- 在 `apps/tokmon-desk` 重新实现当前 Git review 的外部行为；
- libgit2 structured diff；
- unified/split；
- file/hunk stage、unstage、discard；
- DesktopChangeTracker；
- Agent run baseline review；
- 现有 Review 页面、Diff 色值、toolbar 和 pane geometry Golden tests 通过。

### Phase 5：Terminal

- libghostty-vt adapter；
- ConPTY；
- POSIX PTY；
- Skia renderer；
- copy/search/selection；
- 多标签和 shell profiles；
- Terminal 外围 UI 使用旧版 pane、tab、toolbar 和状态样式。

### Phase 6：Agent Browser

- runtime 安装与签名；
- 系统浏览器发现；
- Tokmon Profile；
- MCP config；
- screenshot/stream；
- 用户接管；
- 权限与安全 UI；
- Browser 容器、toolbar、权限条、loading 和 error UI 继承旧版视觉语言。

### Phase 7：切换与清理

- 以 `apps/tokmon-desktop` 为冻结基线执行双实现回归；
- 所有旧版现有页面和状态通过功能、geometry、token 和 Golden Screenshot 一致性门槛；
- 默认切换 RmlUi Desktop；
- 保留短期回滚开关；
- `apps/tokmon-desktop` 及其 Slint 资源继续保留，不在本项目中删除；
- 仅在顶层构建和发行配置中切换默认产物，且不得改写旧目录内部构建文件；
- 更新许可证、构建和发布文档。

## 19. 测试矩阵

### 19.1 平台

| 平台 | GPU | Terminal | Browser |
|---|---|---|---|
| Windows 10/11 | D3D12 | ConPTY + PowerShell/cmd/WSL | Chrome/Chromium/Brave |
| macOS Intel/Apple Silicon | Metal | POSIX PTY + zsh/bash | Chrome/Chromium |
| Linux X11/Wayland | Vulkan | POSIX PTY + bash/zsh/fish | Chrome/Chromium |

### 19.2 视觉与交互一致性

Golden Screenshot 场景至少覆盖：

| 页面/区域 | 必测状态 |
|---|---|
| 应用 Shell | 默认窗口、最小尺寸、resize、左/右 pane 展开与折叠 |
| 初始会话页 | 默认、hover、workspace selector、starter cards |
| Chat | 空、用户消息、Agent 消息、流式、tool call、错误、长 Markdown、代码块 |
| 导航与侧栏 | 搜索、选中、hover、展开/折叠、长名称截断、空列表 |
| Workspace/File Tree | 默认、选中、展开、Git 状态、空目录、大树可视区域 |
| Code Panel | 只读预览、tab、行号、selection、搜索、loading、错误 |
| Review/Diff | 文件列表、unified/split、add/delete、空状态、stage 状态、commit 区 |
| Settings | 每个 tab、输入、下拉、toggle、验证错误、保存成功 |
| Dialog/Overlay | 创建、确认、危险操作、workspace picker、modal backdrop |
| 通用组件 | button、input、tab、pill、card、tooltip、spinner 的全部状态 |

每个场景使用相同 fixture、locale、窗口尺寸和 DPI 分别运行旧版与新版。至少覆盖 1280×800、1440×900、1920×1080，以及 100%、125%、150%、200% DPI。

自动验证规则：

- Palette token 使用 RGBA 精确比较；
- 字体文件 hash、font family、size、weight、line-height 和 fallback 映射精确比较；
- 关键组件 geometry 与旧版相差不超过 1 logical px；
- 非文字、非动画区域的像素差异目标不超过 0.5%；
- 文字边缘允许窄范围抗锯齿 mask，但 glyph bounds、baseline、换行和截断位置必须一致；
- spinner、caret、时间文本等动态区域使用确定性时钟或精确小区域 mask；
- 任何颜色、布局、组件和信息架构差异都不得通过扩大全局容差解决；
- Golden 更新必须由独立 UI 变更批准，技术迁移 PR 只能修复新版实现。

人工验收要求：产品负责人或 UI 设计负责人逐页面并排检查，确认普通用户无法从视觉和任务路径判断底层已从 Slint 切换为 RmlUi/Skia。产品名显示为 `tokmon-desk` 是预先批准的命名差异。

### 19.3 UI 与文本

- 100%、125%、150%、200% DPI；
- 中英文混排；
- CJK、Emoji、combining marks；
- IME composition、candidate rect、commit/cancel；
- RTL 仅做显示级验证，完整交互列为已知限制；
- 字体缺失和 fallback；
- 长消息和快速流式更新。

### 19.4 编辑器和 Diff

- 10 万行文本；
- UTF-8、BOM、CRLF/LF；
- 外部修改冲突；
- undo/redo；
- file rename/delete；
- dirty workspace baseline；
- agent 修改与用户预存修改隔离；
- binary/large file；
- stage/unstage/discard hunk。

### 19.5 Terminal

- ANSI/VT fixture；
- alternate screen；
- vim、less、top 等 TUI；
- CJK double-width 和 Emoji；
- resize/reflow；
- bracketed paste；
- OSC 8；
- Ctrl+C/Ctrl+Z 和进程退出；
- 大量输出和 scrollback 上限；
- Windows/macOS/Linux shell profile。

### 19.6 Browser

- 自动发现；
- executable override；
- 独立 profile；
- headless/headed；
- MCP snapshot/click/fill；
- preview stream；
- user takeover；
- browser crash/restart；
- CDP 拒绝和版本不兼容；
- 无系统浏览器时的可选下载；
- 权限、下载和上传拦截。

### 19.7 数据路径与隔离

- Windows Known Folder、macOS Application Support/Caches/Logs 和 Linux XDG 路径解析；
- Linux 未设置 XDG 环境变量时的标准 fallback；
- 路径解析失败时不得回退到 cwd、用户主目录裸目录或 workspace；
- `DeskAppPaths` 返回规范化绝对路径，并拒绝目录穿越和非预期 symlink 目标；
- Desktop UI 设置与用户级/项目级 `.tokmon` 配置不互相覆盖；
- 清除 cache 不影响 settings、change snapshots、browser profile 和 `.tokmon`；
- workspace 切换不改变 Desktop 应用数据根；
- 日志与 crash recovery 脱敏；
- 多个 `tokmon-desk` 实例的原子写入、锁和冲突行为；
- 卸载、升级和旧版本回滚时的数据兼容与保留策略。

## 20. 性能与验收标准

### 20.1 硬门槛

1. macOS Metal、Windows D3D12、Linux Vulkan 均可启动、resize 和稳定 present。
2. 中文 IME 在普通输入框、编辑器和 Terminal 中正确。
3. 空闲状态不持续高频重绘。
4. 10 万行编辑器能够滚动、定位和基本编辑。
5. 4000 行以上 Diff 不创建同等数量长期 DOM 节点。
6. 大型文件树使用虚拟化和异步展开。
7. Terminal 能运行至少 PowerShell、cmd、zsh、bash、fish 中的平台适用集合。
8. Agent Browser 能使用系统浏览器和独立 Profile 完成 snapshot、click、fill、screenshot。
9. Desktop 重写不要求新增或修改 Daemon 专用能力。
10. 本次实现产生的变更不得落入 `apps/tokmon-desktop`；CI/评审必须检查该目录相对重写起点无差异。
11. `tokmon-desk` 不创建 `tokmon-desk-data` 裸目录，也不把 Desktop 私有状态写入用户级或项目级 `.tokmon`。
12. 所有旧版现有页面、Overlay 和控件状态通过视觉一致性矩阵，Palette、字体、图标、布局和交互没有未经批准的变化。
13. 新版在相同 fixture、viewport 和 DPI 下满足 Golden Screenshot、geometry 和文字布局门槛；不能用放宽容差代替修复。

### 20.2 建议指标

- 首次窗口可交互时间和当前 Slint 版本同量级；
- 空闲 CPU 接近事件循环基线；
- UI thread 不执行超过一个 frame budget 的磁盘/Git/parse 工作；
- Terminal 输出合并更新，不产生逐字节 UI event；
- Browser preview 可以降帧，不拖慢 Chat 和 Editor；
- 内存压力时可释放 glyph、syntax、thumbnail 和 browser preview cache。

视觉一致性属于发布硬门槛，不因性能指标达标而降级为建议项。

## 21. 已知风险与缓解

### RmlUi 无成熟系统级 Accessibility Bridge

这是相对 Electron 最大的长期风险。首版保留清晰焦点、键盘导航和语义模型；若产品要求屏幕阅读器合规，需要单独实现 Windows UIA、macOS NSAccessibility 和 Linux AT-SPI bridge，或重新评估 UI 技术。

### Zep 不是完整 IDE 编辑器

通过 `EditorCore`/`CodeSurface` 隔离，并以中文 IME、大文件、grapheme 和 undo 压测作为进入主线的门槛。

### libghostty-vt C API 尚未稳定

固定 commit、固定 Zig、封装 `TerminalEmulator`，不让 Ghostty 类型进入 UI 和 PTY 层。

### Skia 构建和多 GPU 后端复杂

固定 Skia revision，集中在 `SkiaDevice`，CI 构建三个平台，保存最小后端 feature set。

### 系统 Chrome 版本不可控

运行兼容检查；系统浏览器失败时提示安装受控 Chrome for Testing；不静默下载。

### DesktopChangeTracker 归因不是 Daemon 级审计

明确语义：只在 Desktop 建立 baseline 后声明“本轮 Agent 修改”；Desktop 离线期间只展示普通 workspace changes，不做虚假归因。

### Browser 不是首版无缝内嵌

首版采用 preview + 外部 headed window + takeover。只有真实需求证明值得承担 Chromium/CEF 成本时再实现 CEF Provider。

### Slint 与 Skia 的文字抗锯齿不同

不同平台和渲染器可能产生少量 glyph edge 像素差异。测试只对文字边缘设置窄 mask，同时严格比较字体文件、shaping、glyph bounds、baseline、换行和控件 geometry。不得把文字抗锯齿差异作为更换字体、改变字号或扩大整屏截图容差的理由。

## 22. 最终决策

`tokmon-desk` 的最终方向为：

1. 新版全部实现在 `apps/tokmon-desk`，旧版 `apps/tokmon-desktop` 保持原样；
2. 本项目只做技术重写；新版完整保持旧版 UI 主题、颜色、字体、图标、风格、布局、信息架构和交互设计；
3. 用 SDL3、RmlUi、Skia 重写新版 Desktop UI，不使用框架默认样式替代旧版设计；
4. 使用 D3D12、Metal、Vulkan，不使用 OpenGL；
5. 使用 HarfBuzz + FreeType 复现旧版文字度量和显示，现有 UI 继续使用 MiSans VF；
6. 使用 chmd + Tokmon AST 渲染 Markdown；
7. 使用 Zep Core + Tree-sitter + Skia 构建基础代码编辑器；
8. 文件浏览、Git 审查和 Desktop 修改基线全部属于新版 Desktop；
9. 跨平台 Terminal 使用 `libghostty-vt + ConPTY/POSIX PTY`，不集成完整 Ghostty；
10. Agent Browser 使用开源 `agent-browser + 系统 Chrome/Chromium + Tokmon 独立 Profile`；
11. Agent 调用浏览器复用当前 Iris/Techor MCP，不新增 Daemon 浏览器能力；
12. CEF 只作为未来 Desktop 可选 Provider；
13. `tokmon-daemon` 保持当前能力、协议和职责不变。

该方案在不改变用户当前视觉和交互体验的前提下，完成原生技术栈替换，并在基础安装体积、性能、跨平台一致性、代码工作台能力和未来浏览器扩展之间取得平衡，同时保持 Tokmon 当前 Daemon 架构边界稳定。
