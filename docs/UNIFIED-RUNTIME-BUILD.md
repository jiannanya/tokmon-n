# Tokmon 统一运行时与 Release 构建说明

> 状态：已实现  
> 适用版本：C++20 Tokmon、Windows VS 2022 v143 及可移植构建  
> 更新日期：2026-08-25

## 1. 最终结构

Tokmon 的 CLI、daemon 和 Lens worker 现在统一编译到一个 `tokmon` 可执行文件中。
`tokmond` 和 `tokmon-lens-worker` 仍然是清晰的运行时角色，但不再是需要单独分发的可执行文件。

```text
tokmon.exe（CLI 客户端）
  └─ tokmon.exe（daemon 角色，独立进程）
       └─ tokmon.exe（worker 角色，独立进程）

tokmon-desktop.exe（可选）
  └─ 连接或启动 tokmon.exe（daemon 角色）
```

统一文件没有把三种角色塞入同一个进程。daemon 和 worker 仍有独立地址空间、堆、
线程、句柄和故障边界；变化只是多个进程映射同一份 `tokmon.exe` 文件，从而消除磁盘上的
重复机器码。

## 2. 发行包

### 2.1 默认 CLI 包

默认包的运行时 `bin` 目录只包含：

```text
bin/
  tokmon.exe
```

CLI 命令、daemon 生命周期管理和 worker 监督都由该文件完成。适合终端、服务器、CI、
脚本和编辑器集成。完整安装目录还包含 `share/tokmon` 下的默认配置与 Lens SDK；它们不是
运行时可执行文件。

### 2.2 Desktop 包

Desktop 包的运行时 `bin` 目录包含：

```text
bin/
  tokmon.exe
  tokmon-desktop.exe
  Slint 运行时依赖
  assets/
```

Desktop 不经过 CLI 转发业务请求。它直接通过 Snow IPC 与 daemon 通信；当 daemon 不存在时，
Desktop 启动同目录 `tokmon.exe` 的内部 daemon 角色。

### 2.3 外部 Lens 插件包

官方 Lens 默认静态链接进 `tokmon.exe`，不需要额外 DLL。只有需要 C ABI 动态加载、独立替换
或第三方兼容性验证时，才启用外部 Lens：

```text
bin/
  tokmon.exe
  lenses/
    tokmon-lens-*.dll
```

外部 Lens 是可选扩展，不会改变统一 daemon/worker 架构。

### 2.4 当前 v143 Release 实测

测试环境为 VS 2022 MSVC 19.44、x64、Release、`/O2` 与 LTCG。这里的 MiB 使用
`1 MiB = 1,048,576 bytes`。

| 包 | `bin` 主要内容 | `bin` 大小 | 完整安装目录 |
| --- | --- | ---: | ---: |
| 默认 CLI | 仅 `tokmon.exe` | 3.243 MiB | 3.270 MiB |
| 外部 Lens | `tokmon.exe` + 20 个 Lens DLL | 8.767 MiB | 8.794 MiB |
| Desktop | `tokmon.exe` + Desktop + Slint DLL + assets | 31.401 MiB | 31.428 MiB |

其中默认 `tokmon.exe` 为 3,400,704 bytes。Desktop 体积主要来自当前 26,282,496 bytes
的 `slint_cpp.dll`，不是 daemon/worker 重复文件。

`build/windows-msvc-release/Release/tokmon_core.lib` 仍会作为 MSVC 的静态库中间产物生成，
但不会被复制到安装包。链接器只把最终程序需要的代码合入 `tokmon.exe`，再由 LTCG 与
`/OPT:REF` 去除不可达内容；不能把 `.lib` 的文件大小与发行包大小相加。

## 3. CMake 开关

面向发行的主要开关只有两个：

| 开关 | 默认值 | 用途 |
| --- | --- | --- |
| `TOKMON_BUILD_DESKTOP` | `OFF` | 构建并安装 Slint Desktop |
| `TOKMON_BUILD_EXTERNAL_LENSES` | `OFF` | 构建并安装外部 C ABI Lens DLL |

测试开关仍然保留：

| 开关 | 用途 |
| --- | --- |
| `TOKMON_BUILD_TESTS` | 构建 C++ 与 C ABI 合同测试 |
| `TOKMON_BUILD_SCRIPT_SDK_TESTS` | 启用 Node.js、CPython SDK 合同测试 |

启用测试时，CMake 会自动构建测试需要的 Lens DLL，但除非同时显式打开
`TOKMON_BUILD_EXTERNAL_LENSES`，这些测试 DLL 不会进入安装包。

## 4. 依赖库替换与版本固定

五个目标库均通过 CMake `FetchContent` 接入，并固定到完整 Git 提交 SHA：

| 用途 | 仓库 | 固定提交 |
| --- | --- | --- |
| 测试 | `https://github.com/jiannanya/chtest.git` | `497a52ce53a06855cc7993c338e81e67862866e4` |
| 日志 | `https://github.com/jiannanya/chlog.git` | `d63ceda126cc6165c8cf1101ae5f16db8978882d` |
| JSON | `https://github.com/jiannanya/chjson.git` | `f98fc8d8b228559ec584a331deab911eff6df8ab` |
| YAML | `https://github.com/jiannanya/chyaml.git` | `0586cd91a7b497feb7df1de90da37c6d1728cf1d` |
| HTTP/HTTPS、SSE、WebSocket | `https://github.com/jiannanya/chhttp.git` | `0e11978d228dd1e7be728378c8898f67ab4cf36b` |

“固定提交”表示构建始终取得已经验证过的同一版源码，不会因为远程默认分支后来变化而产生
不可复现的编译结果。升级依赖时应显式修改 SHA，并重新执行 Release 与合同测试。

`chlog`、`chjson`、`chyaml` 和 `chhttp` 以库目标链接进 Tokmon；`chhttp` 的 libuv、
OpenSSL 和压缩库由根目录 vcpkg manifest 固定并随 Windows 产物部署。`chtest` 只在启用
`TOKMON_BUILD_TESTS` 时下载和构建。依赖自身的示例、基准和测试目标默认关闭，避免扩大
Tokmon 的构建与发行产物。

## 5. Windows VS 2022 v143 Release

Windows preset 使用根目录 `vcpkg.json` 的 manifest mode。配置前需要让
`VCPKG_ROOT` 指向已 bootstrap 的 vcpkg；preset 会从第一次配置开始显式加载该
toolchain：

```powershell
$env:VCPKG_ROOT = "C:\\path\\to\\vcpkg"
Test-Path "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```

如果构建目录是在项目引入 chhttp/libuv 之前生成的，旧 cache 可能只有
`CMAKE_TOOLCHAIN_FILE` 字符串、却没有初始化 manifest mode。升级一次时使用：

```powershell
cmake --fresh --preset=windows-msvc-release-desktop
```

之后正常的 `cmake --build --preset=windows-msvc-release-desktop` 会直接工作。

### 5.1 默认统一 CLI

```powershell
cmake -S . --preset=windows-msvc-release
cmake --build --preset=windows-msvc-release --parallel
```

输出：

```text
build/windows-msvc-release/bin/Release/tokmon.exe
```

安装：

```powershell
cmake --install build/windows-msvc-release `
  --config Release `
  --prefix build/package/windows-msvc-release
```

### 5.2 Desktop

先按照 `scripts/bootstrap-slint.ps1` 准备项目本地 Slint SDK，然后执行：

```powershell
cmake -S . --preset=windows-msvc-release-desktop
cmake --build --preset=windows-msvc-release-desktop --parallel
```

安装：

```powershell
cmake --install build/windows-msvc-release-desktop `
  --config Release `
  --prefix build/package/windows-msvc-release-desktop
```

### 5.3 外部 Lens 插件

```powershell
cmake -S . --preset=windows-msvc-release-external-lenses
cmake --build --preset=windows-msvc-release-external-lenses --parallel
```

安装：

```powershell
cmake --install build/windows-msvc-release-external-lenses `
  --config Release `
  --prefix build/package/windows-msvc-release-external-lenses
```

## 6. Portable Debug

macOS 直接链接系统 `Security.framework`，不需要额外的密钥库开发包。Linux 使用
Secret Service，并在配置阶段要求 `pkg-config` 能找到 `libsecret-1 >= 0.20`；例如
Debian/Ubuntu 安装 `pkg-config libsecret-1-dev`，Fedora 安装
`pkgconf-pkg-config libsecret-devel`。若使用 vcpkg toolchain，根目录 manifest 会只在
Linux 目标安装 `libsecret`。运行测试或保存模型密钥时，Linux 用户会话还必须启动
Secret Service（如 GNOME Keyring 或兼容 Secret Service 的 KWallet）。后端不可用时
Tokmon 明确失败，不会改用 YAML、环境旁路或明文文件。

```sh
cmake -S . --preset=portable-debug
cmake --build --preset=portable-debug
ctest --preset=portable-debug
```

该 preset 默认不构建 Desktop，但会构建测试目标及测试所需的外部 Lens 模块。

## 7. 进程启动规则

两个内部参数只用于 Tokmon 自身的进程分派，不属于公共 CLI 接口：

```text
--tokmon-internal-daemon
--tokmon-internal-worker
```

- CLI 和 Desktop 使用 `--tokmon-internal-daemon` 创建 daemon 进程；
- daemon 使用 `--tokmon-internal-worker` 创建受监督 worker 进程；
- 普通用户不需要直接调用这两个参数；
- Snow endpoint、客户端租约、daemon pin/stop 和 worker 管道协议保持不变。

## 8. Release 体积优化原则

Release 保持编译器的速度优先优化，不用体积优先优化替换它：

- MSVC Release 保持 `/O2`；
- 使用 IPO/LTCG 消除跨翻译单元未使用代码；
- 使用 `/Gy`、`/Gw`、`/GF`、`/OPT:REF` 和 `/OPT:ICF`；
- 非测试发行目标移除未使用 RTTI；
- 不使用 UPX 或运行时解压；
- 不静态链接 Windows CRT；
- 不为了体积合并 daemon/worker 的进程地址空间。

因此减少的是磁盘重复内容，不以运行性能、常驻内存或故障隔离为代价。

## 9. 从旧布局迁移

旧发行文件不再需要：

```text
tokmond.exe
tokmon-lens-worker.exe
tokmon-launcher.exe
```

升级安装时应先清理旧安装目录中的这三个文件，避免用户误启动旧版本。CMake 安装到新的空目录时
只会写入当前布局需要的文件。

旧的 `windows-msvc-release-monolithic` 和 `windows-msvc-release-dynamic` preset 已移除。
新脚本应分别改用 `windows-msvc-release` 和
`windows-msvc-release-external-lenses`。旧构建目录不会被自动删除，但不应再作为发行来源。

旧 CMake 选项迁移如下：

| 旧选项 | 新选项或行为 |
| --- | --- |
| `TOKMON_BUILD_UI` | `TOKMON_BUILD_DESKTOP` |
| `TOKMON_BUILD_LENS_MODULES` | `TOKMON_BUILD_EXTERNAL_LENSES` |
| `TOKMON_BUILD_LAUNCHER` | 已删除，不再需要 launcher |
| `TOKMON_DISTRIBUTION_MODE` | 已删除，统一运行时是唯一主布局 |

## 10. 验证清单

每次修改统一入口或发行逻辑后至少验证：

1. 默认 Release 的 `bin` 只有 `tokmon.exe`；
2. `tokmon --help` 正常；
3. worker 内部分派可启动并正常退出；
4. `tokmon daemon start/status/stop` 全部成功；
5. Desktop 可以在没有独立 `tokmond.exe` 时启动并连接 daemon；
6. 完整 C++、C ABI 和脚本 SDK 测试通过；
7. 安装包中不包含测试专用 Lens DLL。
