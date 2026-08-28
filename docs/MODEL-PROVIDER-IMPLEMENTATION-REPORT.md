# 真实大模型平台配置与验证实现报告

## 结论

Tokmon 已从“UI 中存在模型名称但 Janus 固定调用本地 mock”升级为完整真实 provider 光路。CLI 与 Desktop 均可配置、轮换 Key、选择平台、测试真实连接并发起普通对话；Rhea 保持厂商中立，DeepSeek 只是 `openai-compatible` 的一个配置实例。

## 实现内容

### 配置与热重载

- 新增强类型 `ModelProviderConfig` 与 `RuntimeConfig::model_providers/default_model_provider`；
- YAML 新增 `models.default` 和 `models.providers.<id>`；
- 用户级、项目级配置按 id 合并，项目级覆盖用户级；
- 严格 schema、协议/auth 白名单、HTTPS/loopback、预算范围、默认项和 SecretRef 命名空间验证；
- tokmond 原子写入项目 `.tokmon/config.yaml`，并监听两级 config/light-path 文件热重载。

### 平台中立的 Rhea

- provider id 与 wire protocol 完全分离；
- `openai-compatible`、`anthropic`、`gemini` 与 `local` 四类适配器；
- 自定义 endpoint/model/auth，不含 DeepSeek 特判；
- OpenAI-compatible thinking/reasoning 参数、基于 chhttp 的增量 SSE/JSON、tool call、usage、reasoning chunk；
- Cista SecretRef 即时创建精确的一次性 binding，Rhea 只消费 binding 后的短生命周期明文；
- 明文凭据、越权 SecretRef、不安全 endpoint 在 Rhea 边界再次拒绝。

### Janus 与 tokmond

- Janus 不再把 `model` 强制改成 `local-deterministic`；
- tokmond 只从已验证的 RuntimeConfig 解析 provider，不信任客户端提交 endpoint/SecretRef；
- 解析后的非敏感 provider envelope 随 `user.input` Fact 进入光流；
- `model.provider.test` 运行完整 Fact → Lens → Act 光路，不使用旁路 HTTP ping。

### CLI

- `model list/configure/use/secret set/secret delete/test`；
- `run/chat --provider <id>`；
- Windows Console 与 POSIX tty 均关闭 Key 输入回显；重定向 stdin 可用于 CI；
- 不提供明文 Key 命令行参数；provider 列表只返回 `ready/missing`。

### Desktop

- “智能体与模型”页新增平台 ID、协议、endpoint、model、auth、thinking；
- Slint password 输入、安全保存 Key、真实连接测试；
- 配置和 Key 保存后自动刷新 provider 状态；
- 普通会话把当前 provider 选择发送给 tokmond，真实响应继续投影到对话/轨迹。

## 验证证据

- Windows MSVC、C++20、Slint Desktop 全目标编译成功；
- 82/82 CTest 单元、契约、Node、CPython 测试通过；
- 新增配置合并、明文 Key 拒绝、不安全 endpoint 拒绝、Janus 平台中立 envelope 测试；
- Rhea 使用 Python OpenAI-compatible SSE fixture 完成真实 TCP/HTTP 请求、流式 reasoning/content、usage 与 retry 测试；
- 隔离工作区通过 CLI 原子配置 `fixture-cloud`，`model test` 返回 `hello world` 与 3/2 token usage；
- Windows Credential Manager 完成 Key 写入、状态枚举与删除；使用必须鉴权的 loopback provider，经 Cista SecretRef 一次性绑定后成功完成真实 Rhea HTTP 请求；YAML 和工作区全文检索未发现测试 Key；
- daemon 使用 Snow 优雅关闭，未使用进程强杀。

详细使用方式见 [MODEL-PROVIDER-CONFIGURATION.md](MODEL-PROVIDER-CONFIGURATION.md)。
