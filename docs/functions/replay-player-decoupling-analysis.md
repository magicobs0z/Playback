# replay-player-decoupling-analysis — 回放玩家驱动与相机分离分析

## 一、需求

### 1.1 问题

回放世界中的 `ReplaySession::mReplayPlayer` 是本地服务器创建的唯一 `LocalPlayer`。它既承担回放世界的维度、区块和网络处理上下文，又被录制的本地玩家数据与实体位姿回放路径命中。因此，编辑器即使仅需要独立镜头，仍会观察到该对象被快照、回放实体位姿或本地服务器物理更新。

### 1.2 目标

1. 明确区分回放宿主玩家、录制主角实体和编辑器渲染相机。
2. 让自由相机只修改渲染 Camera，不写入宿主玩家。
3. 让录制主角的快照和位姿优先落到可回放的代理实体，而不是宿主 `LocalPlayer`。
4. 保留现有维度切换、区块注入与原生网络 handler 的工作前提。

### 1.3 约束

- `mReplayPlayer` 必须继续可用作 `LegacyClientNetworkHandler`、`Level`、当前 Dimension 和区块加载的宿主。
- 不应把编辑器相机改回 `Player::moveTo` 或 `CameraInstruction` 路径。
- 回放文件中的录制本地玩家 ID 与运行时宿主玩家 ID 不能混同。

## 二、架构

### 2.1 当前数据流

```text
Recorder::writeSnapshot
  -> CreateLocalPlayer(AddPlayer，使用固定的 recorded local-player ID)
  -> ReplaySession::applyPendingSnapshotLocalPlayer
  -> applyGamePacket(AddPlayer)
  -> LegacyClientNetworkHandler

Recorder::writeEntityMovements
  -> ActionMoveEntities
  -> ReplaySession::handleMoveEntities
  -> 按 ActorUniqueID 查找实体
  -> 玩家实体构造 MovePlayerPacket 并交给网络 handler

本地服务器
  -> 宿主 LocalPlayer（mReplayPlayer）逐 tick 物理、碰撞、维度同步

EditorController
  -> ReplaySession::setEditorCameraOverride
  -> LevelRenderer::preRenderUpdate 后写 render Camera
```

### 2.2 根因

`mReplayPlayer` 本身不是独立的“回放主角”，而是 `ClientInstance::getLocalPlayer()` 的缓存指针。`refreshReplayPlayer` 会持续把它重绑到当前回放世界的本地玩家。该玩家必然受本地服务器和客户端物理系统驱动。

回放包驱动发生在两条路径：

1. 快照的 `CreateLocalPlayer` 延后作为 `AddPlayer` 注入；若 recorded ID 与宿主 ID 在重映射、实体碰撞或 handler 语义上合流，宿主可能被 AddPlayer 的初始化/状态包影响。
2. `ActionMoveEntities` 按录制 ID 在 `mReplayPlayer->getLevel()` 查实体；查到 player 时必定构造 `MovePlayerPacket` 并交给原生 handler。若查到的是宿主或录制主角与宿主发生身份合流，宿主直接接受回放位姿。

相机覆盖不会移动 `mReplayPlayer`：它仅在 `LevelRenderer::preRenderUpdate` 调用原生逻辑后，写入 `ClientInstance::getCamera()` 的渲染位置与方向。它不能阻止宿主仍被物理或回放包移动；只是渲染结果不再跟随宿主。

### 2.3 推荐分层

```text
ReplayHostPlayer
  = mReplayPlayer
  = 本地服务器/网络/维度/区块宿主
  = 永不接受 recorded actor 的 AddPlayer、MovePlayer 或状态包

RecordedPrimaryActor
  = 快照 AddPlayer 创建的可见玩家代理实体
  = recorded unique/runtime/uuid 的唯一归属
  = 接收快照、MovePlayer、装备、属性等录制状态

EditorRenderCamera
  = EditorCameraOverrideState
  = 只在渲染帧覆盖 Camera
  = 不持有或修改任一 Actor
```

建议在 `ReplaySession` 增加显式的“宿主身份”和“录制主角身份”分类接口，所有包注入前先做身份路由：

- `AddPlayer`：录制主角永远创建/更新 `RecordedPrimaryActor`，禁止替换或初始化宿主。
- `MovePlayer`：仅允许目标为 `RecordedPrimaryActor`；目标为宿主时丢弃并记录一次诊断。
- 主角状态包：只重映射到代理实体。
- 维度切换、时间、区块：仍由宿主负责。

### 2.4 可分离方案

| 方案 | 做法 | 优点 | 风险/限制 |
| --- | --- | --- | --- |
| A：仅渲染相机分离 | 保持现有 CameraRenderOverride | 已实现，编辑器镜头不触发物理 | 不解决宿主被回放包移动 |
| B：包级宿主保护 | 对 AddPlayer/MovePlayer/主角状态包识别宿主并拒绝 | 改动小，可快速确认根因 | 若快照没有可靠代理实体，主角可能消失 |
| C：主角代理实体 | 固定 recorded ID 始终对应独立 AddPlayer actor；宿主 ID 永不参与重映射 | 根治身份合流，语义清晰 | 要覆盖全部主角引用包和生命周期 |
| D：双客户端/旁观客户端 | 用另一客户端承载镜头 | 隔离最彻底 | 架构、同步和资源成本过高，不建议当前采用 |

推荐以 B 作为诊断护栏、以 C 作为最终实现，A 保持不变。

## 三、执行

### 3.1 分阶段改造

1. 增加 `ReplayActorIdentity`：保存 host local-player 的 UUID/unique/runtime ID，以及 recorded primary actor 的固定 ID/运行时 ID/UUID。
2. `refreshReplayPlayer` 成功后刷新 host 身份；禁止将 host 身份写入 recorded-primary 映射。
3. 在 `applyGamePacket` 解码后、调用 handler 前对 `AddPlayer` 和所有可引用主角的包执行身份路由；检测到目标是 host 时拒绝注入并输出结构化诊断。
4. 在 `handleMoveEntities` 中先判断 `actor == mReplayPlayer`。该情况不得构造 `MovePlayerPacket`；记录为身份冲突。正常情况只向录制代理实体发送移动包。
5. 快照完成后校验 recorded primary actor 存在且不等于 host；否则将快照标记失败，不进入时间线。
6. 增加调试计数：`droppedHostMovePackets`、`droppedHostStatePackets`、`recordedPrimaryActorResolved`，用于确认运行时不存在身份合流。

### 3.2 验证矩阵

| 场景 | 预期 |
| --- | --- |
| 初始快照 | 宿主和录制主角为两个不同 Actor；主角显示在录制位置 |
| 正常播放 | 主角由 `ActionMoveEntities` 移动；宿主不接收 `MovePlayerPacket` |
| 暂停 60 秒 | 宿主可受原生物理影响，但编辑器自由镜头与主角位姿均不漂移 |
| 变维度与 seek | 宿主完成原生 teleport；快照后重新创建/解析主角代理，不发生宿主位移 |
| 相机轨道预览 | render Camera 变化；宿主位置、碰撞状态、网络移动包计数不变 |

### 3.3 不变量

1. `mReplayPlayer` 只代表当前回放世界宿主 `LocalPlayer`。
2. 回放记录的本地玩家永远是独立 actor，绝不与宿主共用 unique ID、runtime ID 或 UUID。
3. `MovePlayerPacket` 的回放目标不能为 `mReplayPlayer`。
4. 编辑器相机覆盖只写渲染 Camera，绝不写 Actor 或 Player。
5. 区块、维度、时间和网络 handler 生命周期仍以宿主玩家为锚点。
