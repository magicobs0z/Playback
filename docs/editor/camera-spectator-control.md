# editor/camera-spectator-control — 本地旁观者轨道控制

> 入口：`src/playback/functions/replay/ReplaySession.*`、`src/playback/editor/controller/EditorController.*`。
> 角色：在隔离回放世界中将本地宿主玩家作为无碰撞旁观者，仅在播放时依据 Camera 轨道控制其姿态；暂停时释放控制以供用户布置机位。
> 实施状态：P0 核心已实现浮点轨道采样、本地宿主姿态接管、能力备份恢复、镜头切换 snap、渲染覆盖 Hook 移除与宿主身份隔离；运行时回放验证待执行。

## 一、需求

### 1.1 功能需求

| ID | 需求 | 优先级 |
| --- | --- | --- |
| CSC-1 | 回放宿主 `mReplayPlayer` 与录制主角代理必须是不同实体，录制移动包不得驱动宿主 | P0 |
| CSC-2 | 选中 Camera 或当前 Sequence 段绑定 Camera 时，播放预览按 `timelineTick + partialTick` 连续采样位置、yaw、pitch | P0 |
| CSC-3 | 启用轨道预览时，宿主进入 `NoClip + MayFly + Flying` 旁观能力状态；退出时恢复原状态 | P0 |
| CSC-4 | 连续轨道段以本地旁观者姿态应用采样结果，禁止再写 CameraInstruction、ECS CameraComponent 或 `mce::Camera` | P0 |
| CSC-5 | 播放开始、seek、Sequence 段硬切和跨 Camera 切换必须执行一次 snap，并强制相机 cut 清理原生插值残留 | P0 |
| CSC-6 | 暂停、无有效 Camera、无有效样本、退出回放世界或停止回放时，释放轨道接管；用户可自由移动并创建关键帧 | P0 |
| CSC-7 | 本轮保持 FOV 状态和采样字段，但不写入 MCBE FOV | P1 |

### 1.2 验收标准

- 两个位置和角度不同的关键帧之间，本地旁观者与画面均按轨道连续移动，无上下抖动。
- 回放录制主角持续显示并按录制数据移动；宿主不会接收录制主角的 `MovePlayerPacket`。
- 播放暂停后可手动移动到任意机位；点击创建关键帧捕获当前本地观察姿态。
- seek 或 Camera Sequence 硬切不会残留前一段的移动插值或旋转拖影。
- 停止回放后，宿主能力、游戏模式与相机控制状态恢复到接管前状态。

## 二、架构

### 2.1 对象职责

```text
RecordedPrimaryActor
  <- AddPlayer / MovePlayer / actor state packets

ReplayHostPlayer (mReplayPlayer)
  <- 回放维度、区块、网络宿主
  <- CameraSpectatorController（仅轨道播放期间）

CameraEntity + timelineTick + partialTick
  -> CameraSampler
  -> CameraSpectatorController
  -> ReplayHostPlayer pose
```

| 单元 | 职责 | 不负责 |
| --- | --- | --- |
| `ReplayActorIdentity` | 保存和区分宿主、录制主角的身份，路由实体包 | 采样 Camera 轨道 |
| `CameraSpectatorController` | 保存接管状态、能力备份、最后姿态与 snap 标记 | 保存编辑器项目数据 |
| `EditorController` | 解析预览 Camera，并以时间轴时间发布采样结果 | 直接操作 `Player` |
| `ReplaySession` | 在回放生命周期和本地 tick 中应用/释放旁观姿态 | 修改关键帧模型 |
| `CameraSampler` | 纯函数计算带小数 tick 的 CameraSample | 引擎对象、能力或网络包 |

### 2.2 状态模型

```cpp
struct SpectatorCameraPose {
    bool active{};
    bool snap{};
    Vec3 position{};
    float pitch{};
    float yaw{};
    float fov{90.0f};
};

struct SpectatorAbilityBackup {
    bool captured{};
    bool noClip{};
    bool mayFly{};
    bool flying{};
};
```

- UI/controller 只发布完整 `SpectatorCameraPose` 快照，不能持有 `Player*`。
- `ReplaySession` 在回放主线程读取该快照；仅当 `active=true` 且未暂停时接管宿主。
- 首次接管时捕获能力备份，并通过 `Player::setAbility` 设置 `NoClip`、`MayFly`、`Flying`。
- `snap=true` 时执行一次即时姿态写入并调用 `LocalPlayer::_forceCameraCut()`；应用完成后自动清除 snap。
- 连续帧仅在位置或角度超过 epsilon 时更新，避免无意义的重复写入。

### 2.3 时间与插值

1. 世界内容仍由 `WorldActorOps::mapTimelineToSourceTick(timelineTick)` 计算并按整数 `sourceTick` 回放。
2. Camera 使用 `double cameraTime = timelineTick + partialTick`，其中 `partialTick` 限制在 `[0, 1]`。
3. `CameraSampler` 用相邻关键帧计算空间路径、时间 easing、最短 yaw 和 pitch；结果纯函数且不读取引擎状态。
4. 不同 Sequence 段仍硬切；边界帧标记 `snap=true`，不混合两台 Camera。

### 2.4 身份路由

1. `refreshReplayPlayer` 更新宿主身份。
2. 快照 `AddPlayer` 创建或更新 `RecordedPrimaryActor`，不得复用宿主 identity。
3. `handleMoveEntities` 在构造 `MovePlayerPacket` 前检查目标 Actor：若目标为 `mReplayPlayer`，拒绝该记录并计入冲突计数。
4. 快照加载完成后验证录制主角代理存在且不等于宿主；失败则中止该快照进入时间线。
5. 宿主仍负责维度、区块、时间与网络 handler，不接收录制主角的状态包。

### 2.5 生命周期与回退

- **播放**：发布有效 pose 后启用旁观能力，并由轨道更新宿主姿态。
- **暂停**：释放姿态接管；不恢复能力，保证用户仍可无碰撞自由布置机位。
- **恢复播放**：重新采样当前时间轴；首帧 snap 后进入连续轨道控制。
- **无样本/取消预览/停止/离开回放世界**：释放接管，恢复备份能力，清空 pose。
- **跨维度**：先走既有 `ensureReplayDimension()`；客户端确认维度完成后再应用 snap pose。

## 三、执行

### 3.1 实现顺序

1. 删除 `CameraRenderOverride` 的 Hook 安装与 `mce::Camera` 写入，避免与本地旁观者形成第二控制源。
2. 在 `ReplaySession` 增加 `ReplayActorIdentity`，并在回放实体移动路径加入宿主拒绝保护。
3. 为 `CameraSampler` 添加小数 tick 采样重载，保持整数 tick 调用的兼容语义。
4. 在 `ReplaySession` 增加 `CameraSpectatorController` 状态发布、能力备份、姿态应用、snap 与恢复逻辑。
5. 调整 `EditorController`：播放时发布 `timelineTick + partialTick` 的 pose；暂停时只捕获/读取当前本地观察姿态，不发布接管。
6. 在 seek、Sequence 段切换、维度完成和 Camera 切换处标记 snap。
7. 添加模型测试覆盖浮点采样、snap 标记、身份拒绝判定和能力状态恢复；构建 DLL 后进行隔离回放手动验证。

### 3.2 已落地实现

1. `CameraSampler::sampleAt` 接受 `double` tick，整数重载继续转发，保证已有调用兼容。
2. `ReplaySession::getCameraTime` 将当前 tick 与播放累积余量组合为 `[tick, tick + 1]` 的采样时间。
3. `EditorController` 在回放 tick 后发布采样姿态；首次生效或 Camera ID 改变时要求 snap。
4. `ReplaySession` 在本地回放线程用 `moveTo` 写入宿主姿态，首次接管时保存并启用 `NoClip`、`MayFly`、`Flying`；清理回放数据时恢复备份能力。
5. `LocalPlayer::_forceCameraCut()` 仅在 snap 帧调用，随后清除快照的 snap 标记。
6. `Playback` 不再安装 `LevelRenderer::preRenderUpdate` 相机渲染 Hook；旧模块保留空兼容入口，未进行任何渲染相机写入。
7. `handleMoveEntities` 的精确和旧版路径均拒绝将移动应用到 `mReplayPlayer`，并记录首次冲突。
8. `applyGamePacket` 的录制实体登记和 `clearRecordedEntities` 的移除路径均不允许污染或删除 `mReplayPlayer`。
9. 轨道采样在回放 `_subTick` 后仅发布；宿主姿态在 `ClientInstance::$update` 的原生更新完成后应用，避免原生本地玩家状态回写覆盖轨道。
10. 姿态应用动态校正 `Player::moveTo` 的位置读回偏移，保证写入后的宿主坐标与 Camera 关键帧坐标一致。
11. `LegacyClientNetworkHandler::handle(MovePlayerPacket)` 在回放注入、轨道接管有效且目标是宿主时短路，禁止普通录制 GamePacket 进入宿主原生位置/旋转插值状态；维度迁移期间放行必要传送。

### 3.3 验证矩阵

| 场景 | 预期 |
| --- | --- |
| 两帧直线路径播放 | 位置、yaw、pitch 连续变化，无 Y 轴抖动 |
| 两帧贝塞尔路径播放 | 位置遵循控制点路径，端点严格命中关键帧 |
| 暂停并移动视角 | 用户可自由移动，未写入关键帧前项目数据不变 |
| 创建关键帧 | 捕获当前宿主位置、`pitch/yaw` 与 FOV |
| seek/硬切 | 立即跳至正确镜头，无过渡残影 |
| 录制主角移动 | 主角代理移动，宿主不被 MovePlayer 包改写 |
| 停止回放 | abilities 恢复、没有残留相机接管 |

### 3.4 不变量

1. 同一时刻只有 `CameraSpectatorController` 或原生自由移动可以控制宿主，不能同时控制。
2. 录制主角的所有移动包目标都不能是 `mReplayPlayer`。
3. 轨道使用 `timelineTick + partialTick`；世界回放仍使用 `sourceTick`。
4. 轨道运行不执行网络 I/O、渲染 Hook 写入、CameraInstruction 或 ECS Camera 写入。
5. 所有临时旁观能力均有可恢复的接管前状态。

### 3.5 运行时测试入口

1. 使用包含录制主角移动的回放文件，从编辑器播放开始，确认录制主角移动而宿主只跟随 Camera 轨道。
2. 暂停后移动宿主到新机位，确认宿主可自由移动且录制主角不被带动；创建关键帧应捕获宿主当前姿态。
3. 在播放中执行 seek、Camera 切换、Sequence 硬切和跨维度切换，确认首帧立即 snap 且无上一镜头残留插值。
4. 停止回放并重新进入普通世界，确认宿主能力恢复，日志中没有宿主移动拒绝以外的实体身份冲突。
