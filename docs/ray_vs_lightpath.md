
告诉我现在tokmon是一个会话表示一条光路，一条光路由许多历史photon组成吗？然后切换会话或者新建会话就代表新的光路对吗，两条光路之间不会由重叠重复的photon对吗？


不是完全这样。当前 `tokmon-n` 里，“会话”“光线”和“光路”需要严格区分：

- 用户会话最接近 `CausalRay / RayId`
- `LightPath` 是当前挂载的 Lens 配置快照
- Photon 是归属于某个 Ray 的事实

## 当前关系

```text
                 当前 LightPath epoch E
             [Lens A → Lens B → Lens C]
                    ▲           ▲
                    │           │
Session A ─→ Ray A ─┴─ Photon A1, A2, A3...
Session B ─→ Ray B ─── Photon B1, B2, B3...
```

多个会话通常共享同一个当前 LightPath，但各自使用不同的 RayId。

### 1. 一个会话是不是一条光路？

严格来说不是。

一个会话对应一条 `CausalRay`，也就是一组具有相同 `RayId` 的 Photon：

```text
Session A
→ RayId = ray-a
→ Photon(ray-a, ...)
→ Photon(ray-a, ...)
→ Photon(ray-a, ...)
```

而 `LightPath` 是：

```text
LightPathSnapshot {
  epoch,
  hash,
  ordered mounted lenses
}
```

它表示当前有哪些 Lens、顺序是什么、各自 generation 是多少。

所以应当说：

> 一个会话对应一条因果光线 Ray；这条 Ray 在每次运行时穿过当前 LightPath。

而不是“一个会话就是一条 LightPath”。

## 2. 新建会话会发生什么？

当前代码中：

```text
runtime.submit(...)
→ engine.begin(...)
→ make_id("ray")
```

会创建一个新的 `RayId`，然后向该 Ray 追加第一条 `user.input` Photon。

所以：

```text
新建会话
≈ 创建新的 CausalRay
≠ 创建新的 LightPath
```

新会话仍然使用当时全局生效的 LightPath。

## 3. 切换会话会发生什么？

切换会话主要是改变客户端的 `active_ray`：

```text
active_ray = ray-a
```

切到另一个会话：

```text
active_ray = ray-b
```

后续输入使用：

```cpp
runtime.submit_to(active_ray, ...)
```

因此切换会话只是选择另一个既有 Ray，不会自动更换 Lens 配置或 LightPath。

## 4. 同一个会话可能经历多个 LightPath epoch

这一点很重要。

假设会话 A 开始时：

```text
epoch 1:
Rhea → Janus → Techor → Calculator
```

后来 Calculator 被卸载：

```text
epoch 2:
Rhea → Janus → Techor
```

同一条 Ray 里可以同时存在：

```text
Photon A1, epoch=1
Photon A2, epoch=1
Photon A3, epoch=2
Photon A4, epoch=2
```

所以：

> 一条会话 Ray 不绑定一个永恒不变的 LightPath；它可以在生命周期中经过多个 LightPath epoch。

历史 Photon 记录自己产生时的 epoch，新操作使用当前 epoch。

## 5. 两个会话之间会不会共享同一个 Photon？

从数据库归属上看，不会。

每个 Photon 只有一个 `ray` 字段：

```cpp
struct Photon {
  PhotonId id;
  RayId ray;
  ...
};
```

因此同一条 Photon 记录只能属于一个 Ray：

```text
Photon P1 → ray-a
```

不能同时属于：

```text
Photon P1 → ray-a + ray-b
```

按 `ray` 查询时，两个不同 Ray 得到的 Photon 行集合是互斥的。

## 6. 但是两个 Ray 可以有重复内容

当前系统没有保证两个会话的 Photon payload 不重复。

例如两个用户都输入：

```text
“读取 README.md”
```

会产生两条不同 Photon：

```text
Photon P1 {
  id: photon-1,
  ray: ray-a,
  payload: "读取 README.md"
}

Photon P2 {
  id: photon-2,
  ray: ray-b,
  payload: "读取 README.md"
}
```

它们内容相同，但：

- PhotonId 不同；
- RayId 不同；
- sequence 不同；
- hash 不同；
- 因果位置不同。

因此准确表述是：

> 两个 Ray 不会共享同一条 Photon 记录，但可能拥有语义或 payload 重复的不同 Photon。

## 7. 分支会形成逻辑重叠

`/branch` 或 Aya child ray 会创建新 Ray：

```text
Parent Ray A
├─ Photon A1
├─ Photon A2
└─ branch.created → child Ray B

Child Ray B
└─ ray.branched {
     parent_ray: A,
     from_sequence: 2
   }
```

当前实现通常不会把 A1、A2 物理复制到 B，而是让 B 记录：

- `parent_ray`
- `from_sequence`
- parent/source hash
- branch provenance

所以它们：

- 物理 Photon 集合不重叠；
- 逻辑因果历史存在继承关系；
- child ray 可以把 parent prefix 视为自己的历史背景；
- 不能因此说两条 Ray 完全独立。

更合理的表示是：

```text
Ray A: [A1, A2, A3]
             │
             └── branch reference
                       │
Ray B:                 [B1, B2]
```

Ray B 没有复制 A1、A2，但逻辑上可能引用它们。

## 8. 全局 hash chain 也会跨 Ray

PhotonStore 的 `sequence` 和 `previous_hash` 是全局的，而不是每个 Ray 各自一条数据库 hash chain。

例如：

```text
sequence 1: ray-a / Photon A1
sequence 2: ray-b / Photon B1
sequence 3: ray-a / Photon A2
```

其全局 hash chain 是：

```text
A1 → B1 → A2
```

但按 Ray 的业务因果视图是：

```text
Ray A: A1 → A2
Ray B: B1
```

当前 Photon 还有一个 `parent` 字段，默认指向该 Ray 上一条 Photon；它和全局 `previous_hash` 是两套关系：

- `previous_hash`：全局账本完整性
- `parent`：业务因果关系，通常在 Ray 内，也允许显式跨 Ray 引用
- `parent_ray/child_ray` payload：分支、子任务关系

## 9. 还有一些 Ray 并不是用户会话

当前实现还会创建：

- `system-ray`
- `mount-ray`
- `command-ray-*`
- `desktop-navigation-ray`
- child ray
- replay/fork ray

所以关系不是：

```text
所有 Ray 都等于用户会话
```

而是：

```text
用户会话是 Ray 的一种
```

系统控制、配置换代、导航和子任务也使用 Ray 来组织 Photon。

## 最准确的定义

可以固定为：

```text
LightPath
= 当前可用 Lens 的有序、不可变、带 epoch 的配置快照

CausalRay
= 一个任务、会话、子任务或控制流程的因果身份

Photon
= 唯一归属于一个 Ray 的已提交事实

Session
= UI/用户层对某个主要 CausalRay 的命名和导航包装
```

因此对你三个问题的直接回答是：

1. **“一个会话表示一条光路吗？”**  
   不是。一个会话主要对应一条 `CausalRay`；多个会话共享当前 `LightPath`。

2. **“新建或切换会话代表新的光路吗？”**  
   新建会话创建新的 Ray；切换会话选择已有 Ray。两者通常都不创建或切换 LightPath。

3. **“两条光线之间不会有重叠重复 Photon 吗？”**  
   同一 Photon 记录只属于一个 Ray，不会物理重叠；但可以有重复内容、跨 Ray 因果引用、parent/child 继承和共享的全局 hash chain。