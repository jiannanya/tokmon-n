# Tokmon 真实大模型平台配置与验证

> 状态：已实现。适用于 C++20 新版 Tokmon、`tokmon`、`tokmond` 与 `tokmon-desktop`。

## 1. 设计目标

真实模型接入仍然遵循 Tokmon 的统一语义：用户输入首先成为 Fact，Janus 根据已验证的配置形成 `model.call` Act，Rhea 将 Act 聚焦为具体协议请求，结果以只能追加的 Photon 回到同一条因果光流。模型平台不是 Nyxia 的特例，也不会绕过 Fact → Lens → Act。

Rhea 不绑定 DeepSeek 或任何单一厂商。配置中的两个身份必须分开理解：

- `id` 是平台或账户的本地名字，例如 `deepseek`、`openrouter`、`company-gateway`；
- `protocol` 是 Rhea 使用的线协议适配器，例如 `openai-compatible`、`anthropic`、`gemini`。

因此 DeepSeek 只是 `openai-compatible` 的一个配置实例。OpenAI、OpenRouter、LiteLLM、vLLM、自建 API 网关以及其他兼容端点均可复用同一适配器；Rhea 内部没有 DeepSeek 专属分支。

## 2. 支持范围

| protocol | 请求/响应能力 | 常用 auth |
|---|---|---|
| `local` | 内置确定性模型，用于离线开发和回归 | `none` |
| `openai-compatible` | Chat Completions、SSE/JSON、reasoning chunk、tool call、usage | `bearer`、`x-api-key`、`none` |
| `anthropic` | Messages 风格请求、SSE、thinking/text/tool use、usage | `x-api-key` |
| `gemini` | generateContent 风格请求、SSE、thought/text/functionCall、usage | `x-goog-api-key` |

Rhea 同时提供超时、取消、响应体上限、重试、指数退避、Retry-After、provider fallback、首 token 与流空闲超时。所有平台响应最终归一化为相同 Photon 类型，Desktop 与 CLI 不需要感知厂商事件格式。

## 3. 最快开始：DeepSeek

以下命令把 provider 的非敏感部分写入当前项目的 `.tokmon/config.yaml`：

```powershell
tokmon model configure deepseek `
  --protocol openai-compatible `
  --endpoint https://api.deepseek.com/chat/completions `
  --model deepseek-v4-pro `
  --auth bearer `
  --thinking `
  --reasoning-effort high `
  --default
```

然后安全输入 Key：

```powershell
tokmon model secret set deepseek
```

终端会关闭输入回显。Tokmon 不接受 `--api-key` 参数，Key 不会出现在命令历史、进程参数、YAML、Photon 或日志中。Windows 上 Key 写入 Credential Manager；YAML 只包含 `secret_ref: model-provider/deepseek`。

执行真实连通性测试：

```powershell
tokmon model test deepseek
```

测试不是旁路 HTTP ping，而是完整的 Fact → Janus → `model.call` → Rhea → Cista 一次性绑定 → provider → Photon 光路。成功时会显示 `assistant.message` 和 `model.usage`；HTTP、鉴权、协议解析或模型错误会产生 `model.failed`/`act.failed` 并返回非零退出码。

普通真实对话：

```powershell
tokmon run --provider deepseek "解释这个项目的光流调度逻辑"
tokmon chat --provider deepseek
```

若已设为默认 provider，可省略 `--provider deepseek`。

DeepSeek 的端点与模型会继续演进，示例不会被编译进 Rhea。请以 DeepSeek 官方的 [Chat Completion API](https://api-docs.deepseek.com/api/create-chat-completion) 和 [模型/价格页面](https://api-docs.deepseek.com/quick_start/pricing/) 为准，只需修改 YAML/CLI 配置，无需重新编译 Tokmon。

## 4. 任意 OpenAI-compatible 或自定义平台

```powershell
tokmon model configure company-gateway `
  --protocol openai-compatible `
  --endpoint https://models.example.com/v1/chat/completions `
  --model company-coder `
  --auth bearer `
  --max-output-tokens 8192 `
  --max-attempts 3 `
  --retry-backoff-ms 500 `
  --default
tokmon model secret set company-gateway
tokmon model test company-gateway "只回复 TOKMON_OK"
```

若兼容服务要求 `x-api-key`，把 `--auth bearer` 改为 `--auth x-api-key`。只有明确无需鉴权的远程服务才能使用 `--auth none`。明文 HTTP 仅允许 `127.0.0.1`、`localhost` 或 `[::1]`，远程 endpoint 必须使用 HTTPS。

用于本地 vLLM/LiteLLM 等开发服务：

```powershell
tokmon model configure local-gateway `
  --protocol openai-compatible `
  --endpoint http://127.0.0.1:8000/v1/chat/completions `
  --model local-model `
  --anonymous `
  --default
tokmon model test local-gateway
```

`--anonymous` 只能直接用于 loopback HTTP；远程匿名服务必须明确设置 `--auth none`，以防误配置导致凭据静默缺失。

## 5. Anthropic 与 Gemini

Anthropic 示例：

```powershell
tokmon model configure anthropic-main `
  --protocol anthropic `
  --endpoint https://api.anthropic.com/v1/messages `
  --model YOUR_ANTHROPIC_MODEL `
  --auth x-api-key `
  --default
tokmon model secret set anthropic-main
tokmon model test anthropic-main
```

Gemini 示例（endpoint 中的模型名应与 `--model` 保持一致）：

```powershell
tokmon model configure gemini-main `
  --protocol gemini `
  --endpoint "https://generativelanguage.googleapis.com/v1beta/models/YOUR_GEMINI_MODEL:streamGenerateContent?alt=sse" `
  --model YOUR_GEMINI_MODEL `
  --auth x-goog-api-key `
  --default
tokmon model secret set gemini-main
tokmon model test gemini-main
```

## 6. YAML 格式

CLI 原子生成的项目配置如下：

```yaml
models:
  default: company-gateway
  providers:
    company-gateway:
      protocol: openai-compatible
      endpoint: https://models.example.com/v1/chat/completions
      model: company-coder
      secret_ref: model-provider/company-gateway
      secret_env: COMPANY_GATEWAY_API_KEY
      auth: bearer
      enabled: true
      allow_anonymous: false
      thinking: false
      reasoning_effort: ""
      max_output_tokens: 8192
      max_attempts: 3
      retry_backoff_ms: 500
```

`secret_env` 是可选的非交互式引导来源，不是保存 Key 的位置。名称必须是大写环境变量格式（例如 `OPENCODE_API_KEY`），YAML 仍必须提供严格命名空间化的 `secret_ref`。`tokmond` 启动时先读取当前进程环境；Windows 还会读取当前用户和本机环境变量注册表，以支持在 Desktop/终端启动之后才写入的变量。读取成功后立即导入操作系统凭据库，后续调用只使用 `secret_ref`，临时明文缓冲区会被覆写。环境变量不存在时不会降低为匿名调用。

OpenCode 验收配置示例：

```yaml
models:
  default: opencode
  providers:
    opencode:
      protocol: openai-compatible
      endpoint: https://opencode.ai/zen/v1/chat/completions
      model: x-preview-f-free
      secret_ref: model-provider/opencode
      secret_env: OPENCODE_API_KEY
      auth: bearer
      enabled: true
      allow_anonymous: false
      thinking: false
      reasoning_effort: high
      max_output_tokens: 4096
      max_attempts: 2
      retry_backoff_ms: 500
```

`x-preview-f-free` 不接受 `medium`，应使用 `low`、`high` 或 `max`。本次 Windows 现场验收使用 `high`。完整 CLI/Desktop 记录见 [OPENCODE-DESKTOP-ACCEPTANCE-REPORT.md](OPENCODE-DESKTOP-ACCEPTANCE-REPORT.md)。

用户级 `~/.tokmon/config.yaml` 与项目级 `<workspace>/.tokmon/config.yaml` 使用同一 schema。先合并用户级，再按 provider id 合并项目级；项目可以覆盖 model、endpoint 或预算，但 SecretRef 必须严格等于自己的 `model-provider/<id>`，不能引用其他 provider 或其他透镜的凭据。`models.default` 必须指向存在且启用的 provider。

配置由 `tokmond` 使用临时文件和原子替换发布。daemon 同时监听用户级/项目级 `config.yaml` 与 `light-path.yaml`，外部编辑保存后会触发验证和热重载；非法字段、非 HTTPS 远程地址、越权 SecretRef 或越界预算会拒绝整个候选配置。

## 7. Desktop 操作

打开“设置 → 智能体与模型”：

1. 填写平台 ID、协议适配器、HTTPS Endpoint、模型和鉴权方式；
2. 点击“保存平台配置”，Desktop 通过 Snow 交给 `tokmond` 原子保存并热重载；
3. 在密码输入框填写 Key，点击“安全保存 Key”；输入框立即清空，Key 只写入系统凭据库；
4. 点击“测试真实连接”，结果会同时显示在对话区和轨迹区；
5. 后续普通会话自动使用这个默认 provider。

密码框使用 Slint password 输入类型。Desktop 与 CLI 共享同一个 tokmond 和同一个 provider 配置，不需要先启动 launcher，也不需要重复配置。

## 8. CLI 管理命令

```text
tokmon model list
tokmon model configure <id> --protocol <protocol> --endpoint <url> --model <model> [options]
tokmon model use <id>
tokmon model secret set <id>
tokmon model secret delete <id>
tokmon model test <id> [text]
tokmon run --provider <id> <message>
tokmon chat --provider <id>
```

`model list` 只报告 `ready`/`missing`，绝不读取或显示 Key。轮换 Key 只需再次执行 `model secret set <id>`；删除使用 `model secret delete <id>`。

## 9. 安全与因果不变量

- API Key 明文只存在于短生命周期的本地 Snow 请求和 Rhea 请求缓冲区；CLI 原始输入与 Rhea 专用缓冲区在使用后主动覆写，通用传输对象在请求结束后立即销毁。
- Cista 从操作系统凭据库创建与 `act_hash + target + generation + epoch` 精确绑定、最多两分钟有效的一次性凭据绑定；Rhea消费后立即失效。
- YAML、Photon、Act、Surface、日志和 CLI 响应仅携带 SecretRef、`credential_present` 或 `opaque-binding`，不携带 Key。
- 配置拒绝 `api_key`、`secret_value` 等未知明文字段；Rhea 再次拒绝携带明文凭据的 Act。
- SecretRef 被 provider id 命名空间约束，项目配置不能借模型调用读取其他秘密。
- endpoint 在配置层与 Rhea 层双重验证：远程只允许 HTTPS，HTTP 仅允许 loopback。
- 模型调用的请求、流式 chunk、usage、完成或失败均追加为新 Photon；历史 Photon 从不编辑、撤销或覆盖。

## 10. 故障定位

先运行：

```powershell
tokmon model list
tokmon doctor
tokmon model test <id>
```

常见结果：

- `credential missing`：执行 `tokmon model secret set <id>`；
- `secret reference was not found`：系统凭据库中没有对应条目，或当前 Windows 用户不同；
- `401/403`：Key、鉴权 header 或平台账户权限错误；
- `404`：endpoint 路径或 model id 错误；
- `429/5xx`：Rhea 会按配置重试，最终仍失败时追加 `model.failed`；
- `model endpoint must use HTTPS or loopback HTTP`：远程地址使用了明文 HTTP；
- `SecretRef must be scoped to its own id`：手写 YAML 引用了其他命名空间；
- 没有 `assistant.message` 但有 `model.failed`：检查对应 Photon 的已脱敏错误、HTTP 状态和 attempt。
