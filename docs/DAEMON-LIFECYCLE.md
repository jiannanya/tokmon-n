# Tokmon 客户端租约与 `tokmond` 生命周期

> 产品题记：**A Lens to Them All**  
> 实现标准：C++20、`tl::expected`、spdlog  
> 更新日期：2026-08-23

## 1. 目标

`tokmond` 按规范化 workspace 隔离，每个 workspace 最多一个实例。客户端不再以“谁创建进程谁就无条件杀死进程”判断所有权，而是向 daemon 注册带超时的客户端租约。这样既能让 Desktop 关闭后回收对应 daemon，也不会误杀另一个 Desktop、CLI 会话或正在执行的模型/Act。Desktop 的项目/会话树可以在运行期切换工作空间：导航工作空间租约保持树可保存，当前活动项目另持一个可交接租约。

## 2. 已实现的退出策略

| 入口 | 退出后的行为 |
| --- | --- |
| `tokmon-desktop` | 释放 Desktop 租约；最后一个客户端离开且活动工作完成后，等待 250 ms 即停止对应 workspace 的 `tokmond` |
| Desktop 异常崩溃 | 心跳停止；6 s 租约过期后执行同样的安全空闲退出 |
| `tokmon chat` / `tokmon stdio` | 会话期间持续续租；会话结束后等待 250 ms 空闲退出 |
| CLI 一次性命令 | 完成后保留 15 s 复用窗口，便于连续运行 `history`、`doctor`、`lens` 等命令；窗口结束后自动停止 |
| `tokmon daemon start` | 把 daemon 标记为用户显式常驻，不再接受租约触发的自动退出 |
| `tokmon daemon stop` | 显式优雅停止；等待当前持有 Nyxia 运行时互斥量的工作完成后退出 |
| `tokmon daemon status` | 只探测，不启动、不注册租约、不改变退出计时 |

因此 CLI 同时覆盖了两类使用习惯：普通命令不永久留下后台进程，短时间命令串又不必反复冷启动；确实需要常驻时则用显式、可理解的 `daemon start/stop`。

## 3. Snow 生命周期协议

生命周期库通过 Snow intent 使用四个内部动作：

- `daemon.client.attach`：注册 `client_id`、客户端类型、租约 TTL 与空闲退出策略；
- `daemon.client.heartbeat`：续租，默认每个 TTL 的三分之一发送一次；
- `daemon.client.detach`：正常退出并请求在指定空闲窗口后停止；
- `daemon.pin`：记录用户显式常驻决定并取消待执行的自动退出。

`DaemonClientLease` 是 C++ RAII 对象。attach 成功后启动 `std::jthread` 心跳；析构或显式 `detach()` 时停止心跳并发送 detach。所有失败均以 `tl::expected<T, tokmon::Error>` 返回，不以异常控制正常流程。

## 4. 安全退出条件

租约触发的停止必须同时满足：

1. daemon 未被显式 pin；
2. 当前没有有效 Desktop 或 CLI 租约；
3. 空闲宽限时间已经结束；
4. Nyxia 运行时互斥量可立即取得，即没有模型调用、光流推进、配置 reconcile 或 Act 正在执行。

维护循环每 250 ms 清理过期租约并检查上述条件。停止决定在生命周期锁内二次确认，从而避免“检查为空之后恰好有新客户端 attach”的竞态。显式 stop 仍经过运行时串行边界，所以不会在半个 Act 中截断因果事实提交。

## 5. 多客户端语义

多个客户端连接同一 workspace 时各自持有独立 `client_id`。任意 Desktop 的关闭只删除自己的租约；只有最后一个客户端离开才会进入退出条件。若 Desktop 关闭时 CLI 正在执行，CLI 心跳和运行时活动都会阻止退出。若用户此前执行过 `tokmon daemon start`，所有自动退出请求都会被 pin 抑制，直到显式 `tokmon daemon stop`。

### 5.1 Desktop 跨工作空间交接

项目节点保存绝对 `workspace`；会话节点为空时继承项目，非空时覆盖。选择不同工作空间的节点时，Desktop 先为目标目录计算 endpoint、确保 daemon ready 并 attach 新租约，成功后才发布目标 endpoint 和释放旧活动租约。导航树的保存始终发往启动 Desktop 时的导航 endpoint，避免活动项目变化导致树配置分裂。

关闭 Desktop 时，controller 析构会先停止 Snow worker，再释放活动工作空间租约；主流程随后释放导航工作空间租约。两者相同时只存在主租约。现场验证从 `workspace_override` 切回 `workspace_alt` 后前者退出，关闭 Desktop 后 `workspace_alt` 与导航工作空间 daemon 均退出。

## 6. 验收结果

- C++20 Windows UI 构建成功；
- 自动化测试 `84/84` 通过，其中包含 attach、heartbeat、detach 与 pin 的 Snow 合约测试；
- CLI 一次性命令结束后 daemon 立即可复用，17 s 后已停止；
- 显式 `daemon start` 后等待 17 s 仍保持运行，`daemon stop` 可正常结束；
- Desktop 收到正常窗口关闭后进程正常退出，2 s 后对应 daemon 已停止；
- 强制终止 Desktop 模拟崩溃，8 s 后过期租约已使对应 daemon 停止。
- Desktop 与交互 CLI 同时连接时，Desktop 关闭后 daemon 保持运行；CLI 最后退出后 daemon 才停止。

关键实现：

- `sdk/cpp/include/tokmon/daemon_lifecycle.hpp`
- `protocol/daemon_lifecycle.cpp`
- `apps/tokmond/main.cpp`
- `apps/tokmon-cli/main.cpp`
- `apps/tokmon-desktop/main.cpp`
- `tests/unit/core_tests.cpp`
