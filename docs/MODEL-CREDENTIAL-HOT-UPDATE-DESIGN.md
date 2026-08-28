# Tokmon 模型凭据配置与热更新设计

> 状态：已实现，并于 2026-08-28 完成 Windows Release Desktop 构建、完整单元测试与 CLI 集成验证。

## 1. 目标

Tokmon 的模型配置需要同时满足以下目标：

- 用户可以直接编辑 YAML，也可以通过 CLI 或 Desktop 修改配置并原子写回 YAML；
- 配置中保留可选的 `secret_env`，用于指定备用环境变量名称；
- 配置中不再暴露或要求用户维护 `secret_ref`；
- 用户通过 CLI 或 Desktop 更新 API Key 后，无需重启 daemon，下一次模型请求自动使用最新 Key；
- API Key 明文不进入 YAML、Photon、日志、异常文本、进程参数或上游请求正文；
- Windows、macOS 和 Linux 使用一致的业务语义；
- 密钥写入、轮换和绑定遵循 “everything is a lens”，由 Cista 负责。

本文描述已落地的设计，不定义旧格式兼容行为。Tokmon 尚处于开发阶段，旧的模型
`secret_ref` 字段将直接拒绝，不保留迁移或兼容分支。

## 2. 用户配置

模型配置保留 `secret_env`，删除用户可见的 `secret_ref`：

```yaml
models:
  default: openrouter
  goes:
    openrouter:
      protocol: openai-compatible
      endpoint: https://openrouter.ai/api/v1/chat/completions
      model: openai/gpt-5
      auth: bearer
      secret_env: OPENROUTER_API_KEY
      stream: true

      provider:
        order:
          - Cerebras
          - Groq
        allow_fallbacks: true
```

字段语义如下：

- `models.default` 是默认配置名称；
- `models.goes.<name>` 的 map key 是 Tokmon 内部配置名称；
- `protocol` 选择上游协议适配器；
- `provider` 等非保护字段作为动态参数进入上游请求正文；
- `secret_env` 是可选的备用环境变量名称，而不是 API Key；
- API Key 明文永远不写入 YAML。

`secret_env` 必须符合统一的环境变量名称约束，例如：

```text
^[A-Z_][A-Z0-9_]{0,127}$
```

对于 `auth: none` 或明确允许匿名访问的本地模型，不执行凭据解析。

## 3. 内部密钥标识

模型密钥的内部 ID 由配置名称确定性生成：

```text
<配置名称> → model-secret-library/<配置名称>
```

例如：

```text
openrouter → model-secret-library/openrouter
deepseek   → model-secret-library/deepseek
```

该 ID：

- 不出现在用户 YAML 中；
- 不需要 CLI 或 Desktop 用户填写；
- 不进入上游模型请求；
- 不能由任意请求覆盖；
- 只用于 Cista 与操作系统凭据后端之间的内部寻址。

配置名称继续使用严格格式校验，从而避免跨配置引用和命名空间逃逸。

## 4. 凭据解析优先级

每一个新的模型请求都重新解析当前凭据，不长期缓存 API Key：

```text
1. 系统密钥库中存在 model-secret-library/<name>
   └─ 使用系统密钥库中的 Key

2. 系统密钥库中不存在该 Key，且配置了 secret_env
   └─ 读取 daemon 当前进程中的环境变量

3. 两者都不存在
   └─ 明确报告“尚未配置 API Key”
```

系统密钥库必须优先于环境变量。这样可以避免旧环境变量在配置重载时覆盖用户刚从
Desktop 或 CLI 保存的新 Key。

`secret_env` 是备用来源，不自动导入或覆盖已经存在的密钥库记录。环境变量值只在最终
凭据解析阶段进入短时内存，不写入配置和因果记录。

## 5. YAML 热更新

用户级和项目级 YAML 继续由 daemon 监视。用户可以直接修改文件，也可以通过 CLI 或
Desktop 修改普通配置并写回同一份 YAML。

```text
YAML 文件发生变化
 → 重新读取
 → 完整 schema 与安全校验
 → 原子发布新的 RuntimeConfig
 → 后续请求使用新配置
```

可热更新的字段包括：

- `models.default`；
- 配置名称下的 `protocol`、`endpoint`、`model`、`auth`；
- `secret_env`；
- `stream`、`thinking`、重试、超时与预算；
- `provider` 等动态上游请求参数。

正在执行的请求继续使用创建该 Act 时的配置。新配置发布后创建的请求使用新配置，避免
在一个请求中途改变 endpoint、模型或鉴权来源。

如果系统密钥库中已有对应 Key，修改 `secret_env` 只会改变备用来源，不会替换当前使用
的密钥库 Key。

## 6. Desktop 用户体验

Desktop 模型设置页将普通配置和密钥明文明确分开：

```text
配置名称：       openrouter
Endpoint：       https://openrouter.ai/api/v1/chat/completions
Model：          openai/gpt-5
备用环境变量名： OPENROUTER_API_KEY

API Key：        ••••••••••••••••    [更新 API Key]
```

### 6.1 保存普通配置

“保存配置”修改 endpoint、model、auth、stream、`secret_env` 等普通字段，原子写回项目
YAML，并触发 daemon 配置热加载。该操作不读取、显示或写入 API Key。

### 6.2 更新 API Key

“更新 API Key”不修改 YAML：

```text
Desktop
 → Snow 本地敏感请求
 → Cista secret.rotate
 → 更新系统密钥库
 → 返回成功
 → 清空 Desktop 密码输入框
```

成功状态使用明确文案：

```text
API Key 已更新，下次请求立即生效
```

Desktop 只显示凭据状态和来源：

```text
凭据状态：已配置
凭据来源：系统密钥库
```

或者：

```text
凭据状态：可用
凭据来源：环境变量 OPENROUTER_API_KEY
```

Desktop 不显示 API Key 原文、内部密钥 ID 或可逆指纹。

## 7. CLI 用户体验

### 7.1 修改配置和 `secret_env`

CLI 的模型配置命令支持 `--secret-env`：

```powershell
tokmon model configure openrouter `
  --protocol openai-compatible `
  --endpoint https://openrouter.ai/api/v1/chat/completions `
  --model openai/gpt-5 `
  --auth bearer `
  --secret-env OPENROUTER_API_KEY `
  --default
```

该命令原子写回 YAML，daemon 自动热加载。清除备用环境变量使用显式选项：

```powershell
tokmon model configure openrouter --no-secret-env
```

### 7.2 创建或更新 API Key

```powershell
tokmon model secret set openrouter
```

CLI 关闭终端输入回显，从标准输入读取 Key，不接受 `--api-key` 命令行参数：

```text
API Key:
API Key 已更新，下次请求立即生效
```

同一个命令同时承担首次创建和后续轮换，用户不需要区分 create 与 rotate。

### 7.3 删除密钥库 Key

```powershell
tokmon model secret delete openrouter
```

如果配置的 `secret_env` 在 daemon 当前环境中存在，删除密钥库记录后，后续请求会使用
环境变量，并返回清晰提示：

```text
密钥库凭据已删除；当前仍可通过 OPENROUTER_API_KEY 鉴权
```

如果环境变量也不存在，则后续鉴权请求明确失败。

## 8. Cista 光路

密钥更新不能由 daemon 绕过 Cista 直接写入凭据后端。

### 8.1 密钥写入

推荐的敏感输入光路为：

```text
Desktop/CLI
 → Snow 同用户本地传输
 → daemon 将明文放入可清零的短时 SecretBuffer
 → 生成一次性、不透明的敏感输入句柄
 → secret.rotate Act 只携带配置名称、purpose 和输入句柄
 → Cista 消费句柄并推导 model-secret-library/<name>
 → 写入系统密钥库
 → 销毁句柄并清零 SecretBuffer
 → 追加不含明文的 secret.rotated Photon
```

普通 Act、Photon、Surface、日志和诊断包中不允许出现 API Key。一次性输入句柄不能被重复
消费，且必须绑定当前 epoch、目标 generation、用途和短期限。

### 8.2 模型请求

```text
model.call(name=openrouter)
 → Cista 根据 name 推导内部密钥 ID
 → 按“密钥库优先、环境变量备用”解析当前 Key
 → 创建绑定 Act hash、epoch、target generation 和 purpose 的一次性 binding
 → Rhea 在最终 HTTP 边界解析 binding
 → 按 auth 模式注入 Authorization 或对应 header
 → binding 使用后立即失效
```

模型配置和 model.call 不再携带用户配置的 `secret_ref`。

## 9. 跨平台存储后端

Cista 使用当前平台的系统凭据后端：

| 平台 | backend 元数据 | 实际后端 |
|---|---|---|
| Windows | `windows-credential-manager` | Windows Credential Manager |
| macOS | `macos-keychain` | Keychain Generic Password |
| Linux | `linux-secret-service` | Secret Service/libsecret |

所有后端使用相同的内部密钥 ID 和业务语义。后端不可用时明确失败，不得退化为 YAML、
普通文件或其他明文存储。

## 10. 热更新时序保证

API Key 更新成功响应只能在系统密钥库写入成功之后返回。对于同一模型配置，Cista 应串行
处理密钥轮换和新 binding 的创建，从而给出明确顺序：

```text
请求 A 已创建 binding ───────── 使用旧 Key
API Key 更新开始
Cista 完成系统密钥库写入
API Key 更新成功
请求 B 创建 binding ─────────── 必须使用新 Key
```

更新开始前已经创建的 binding 保持原值，确保在途请求的因果上下文不被中途修改。更新
成功后创建的新 binding 必须读取最新值。

这使 daemon、会话、LightPath 和 Desktop 都无需重启，且不会破坏请求的时空组合边界。

## 11. 环境变量边界

`secret_env` 的字段值（环境变量名称）可以通过 YAML、CLI 或 Desktop 热更新，但外部进程
无法跨平台修改已经运行的 daemon 的进程环境。

例如，在另一个 PowerShell 中执行：

```powershell
$env:OPENROUTER_API_KEY = "new-key"
```

只影响该 PowerShell 及以后创建的子进程，不会更新已经运行的 daemon。即使 daemon 每次
请求重新调用 `getenv()`，通常仍然只能看到它启动时继承的值。

因此本设计明确规定：

- `secret_env` 用于首次启动、CI、容器部署和无交互备用凭据；
- 运行时 API Key 轮换通过 Desktop 或 CLI 进入 Cista；
- 不实现不可靠的平台特定环境变量轮询；
- 不引入额外 secret 文件、同步命令或配置层级。

## 12. 状态与错误

模型配置查询只返回安全状态：

```yaml
name: openrouter
credential_ready: true
credential_source: vault   # vault | environment | missing | not-required
secret_env: OPENROUTER_API_KEY
```

不得返回 API Key、内部密钥 ID或可逆密钥指纹。

错误规则如下：

- API Key 为空时拒绝更新；
- 远程鉴权配置缺少两个凭据来源时明确失败；
- 不允许静默退化为匿名请求；
- 不允许一个配置访问另一个配置的密钥；
- 不安全远程 HTTP endpoint 继续拒绝；
- 配置中的模型 `secret_ref` 作为已删除字段直接拒绝；
- 系统凭据后端失败时返回明确的 backend 错误。

## 13. 实现改动范围

### 13.1 配置层

- 从模型配置 schema 和 `ModelProviderConfig` 删除 `secret_ref`；
- 保留并验证 `secret_env`；
- 根据配置名称内部生成 `model-secret-library/<name>`；
- 解析旧模型 `secret_ref` 时直接报错；
- 模型上下文传播 `name` 与可选的 `secret_env`，不再传播模型 `secret_ref`。

### 13.2 CLI 与 Desktop

- CLI `model configure` 增加 `--secret-env` 和 `--no-secret-env`；
- Desktop 增加或保留可编辑的“备用环境变量名称”；
- 两个入口都把普通配置原子写回 YAML；
- 两个入口的 API Key 更新都进入同一 Cista 热更新光路；
- 更新成功后刷新 `credential_ready` 和 `credential_source`。

### 13.3 daemon、Cista 与 Rhea

- daemon 不再直接使用用户配置的 SecretRef；
- daemon 不再在普通配置重载时用环境变量覆盖已有密钥库 Key；
- Cista 负责内部 ID 推导、写入、删除、binding 与元数据 Photon；
- Rhea 按配置名称请求 binding，不直接接受任意密钥引用；
- 新请求始终创建新的 binding，以取得更新后的当前 Key。

## 14. 验收测试

实现验收覆盖以下测试：

1. CLI 修改 `secret_env` 后正确原子写回 YAML；
2. Desktop 修改 `secret_env` 后正确原子写回 YAML；
3. 直接编辑 YAML 后 daemon 自动热加载；
4. 模型配置不再生成或接受 `secret_ref`；
5. 内部 ID 严格为 `model-secret-library/<name>`；
6. 密钥库为空时可使用 `secret_env`；
7. 密钥库值优先于环境变量值；
8. 同一 daemon 中通过 Desktop/CLI 更新 Key 后，下一次请求使用新 Key；
9. 更新前已经创建的 binding 仍保持旧 Key；
10. 删除密钥库 Key 后按规则切换到环境变量或明确失败；
11. 配置名称不能越权访问其他模型配置的密钥；
12. YAML、Photon、Act、日志、UI 状态和错误中不存在 API Key 明文；
13. Windows Credential Manager 真实写入、轮换、读取和删除通过；
14. Windows MSBuild Release 全量测试通过；
15. `cmake --build --preset=windows-msvc-release-desktop` 通过；
16. macOS Keychain 与 Linux Secret Service 在各自平台完成构建和现场验收。

## 15. 最终用户心智模型

用户只需要理解两个概念：

```text
secret_env：可选的备用环境变量名称，属于普通配置
API Key：通过 Desktop/CLI 保存到系统密钥库，可在运行时立即轮换
```

用户不需要理解或配置 SecretRef。修改 YAML、CLI 配置或 Desktop 配置会自动热加载；更新
API Key 后，下一次请求自动使用最新值，不需要重启 daemon，也不需要额外同步命令。
