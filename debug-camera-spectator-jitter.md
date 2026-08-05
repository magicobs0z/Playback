# 调试记录：camera-spectator-jitter

状态：OPEN

## 现象

- Camera 轨道播放时镜头抖动。
- Camera 轨道采样的角度没有生效。

## 假设

1. `moveTo` 的旋转参数顺序或引擎解释与采样的 pitch/yaw 顺序不一致，导致姿态未正确应用。
2. 本地宿主的原生移动/物理在回放 tick 后覆盖轨道姿态，导致位置或角度来回写入。
3. 轨道预览仅在 `MultiPlayerLevel::_subTick` 边界应用，渲染帧使用原生插值，导致视觉抖动。
4. Camera 采样结果本身有效，但 `LocalPlayer` 的最终位置和旋转与发布值不一致。
5. `snap` 或 Camera 切换状态被连续重置，导致每帧发生相机 cut。

## 证据计划

- 记录采样姿态、发布 snap、应用前后宿主姿态、能力状态和连续帧差值。
- 复现一次两个位置与角度不同的关键帧之间的轨道播放。

## 运行时证据

- `trae-debug-log-camera-spectator-jitter.ndjson:1-3`：首次 snap 后，`moveTo` 写入的 yaw/pitch 与目标一致，但实际 Y 从 `100.620` 变为 `102.240`。
- `trae-debug-log-camera-spectator-jitter.ndjson:4-5`：下一次应用前实际角度已变为 `14.030/-1.417`，应用后恢复为轨道目标 `0.276/-0.028`。
- `trae-debug-log-camera-spectator-jitter.ndjson:8-11`、`:10-11`：应用前实际位置重复回跳到 `40.260/100.620/-12.508`，而应用后立即回到轨道目标。
- `trae-debug-log-camera-spectator-jitter.ndjson:46-47`、`:50-51`、`:58-59`：回跳持续发生在非 snap 帧；snap 仅出现在首帧，排除连续 camera cut。

## 结论

- 已确认：轨道采样和 `moveTo` 的 yaw/pitch 参数映射正确；每次调用后 Actor 实际旋转与目标一致。
- 已确认：轨道写入之外仍有原生回放/本地玩家状态将宿主位置和旋转回写为旧值，造成轨道值与旧值交替出现，直接导致抖动和方向无法稳定显示。
- 已确认：当前调用路径将目标 Y 写为 `100.620`，但 `moveTo` 后 Player 位置变为 `102.240`；姿态控制必须使用与本地相机一致的眼部/宿主坐标语义，不能假定 `moveTo` 的读回位置等于输入位置。
- 已排除：连续 snap 或 yaw/pitch 参数反转是本次问题的主因。

## 后续修复方向

1. 将轨道姿态应用移动到本地玩家原生状态更新完成之后，并在同一最终阶段同步位置、旋转和前一帧插值状态。
2. 不再依赖只在 `_subTick` 尾部的一次 `moveTo`；需找到 LocalPlayer 的最终 tick/渲染前姿态同步入口，确保原生状态不会在镜头写入后回滚。
3. 保持 `snap` 仅用于 seek、切镜和恢复播放；连续轨道帧改为一致的本地姿态与插值状态更新，不使用逐帧 camera cut。

## 修复

- 轨道采样保留在回放 `_subTick` 后发布，最终姿态写入移至 `ClientInstance::$update` 的 `origin()` 返回后，使其位于可确认的客户端原生更新末端。
- 姿态写入动态测量并抵消 `moveTo` 的位置读回偏移；首次测量后立即重写一次校正坐标，后续帧复用最新偏移。

## 性能回归

- 修复后播放速度降至约 `0.1x`，但镜头抖动消失。
- 已确认原因：诊断阶段在 `ClientInstance::$update` 主线程的每次姿态应用前后同步执行 WinHTTP 建连和请求；这会直接降低客户端更新与回放 tick 的吞吐。
- 已移除全部调试 HTTP、WinHTTP 依赖和主线程网络 I/O；姿态提交与偏移校正保持不变。

## 状态隔离

- 运行时复测表明，后置姿态写入不足以消除原生宿主状态覆盖。
- 在 `LegacyClientNetworkHandler::handle(MovePlayerPacket)` 增加最终边界：仅拒绝回放注入、轨道接管有效、目标 runtime ID 为本地宿主且不处于维度迁移中的包；其他玩家、其他包、真实网络与维度迁移均放行。

## 渲染帧权威姿态试验

- 姿态提交移至 `LocalPlayer::frameUpdate` 原生调用返回后，确保每个渲染帧最后写入最新轨道姿态。
- 轨道接管期间短路 `localPlayerTurn` 与 `_applyTurnDelta`，隔离本地鼠标旋转；暂停、维度迁移或无有效轨道时放行原生输入。
