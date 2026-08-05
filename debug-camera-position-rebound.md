# Debug Session: camera-position-rebound
- **Status**: [OPEN]
- **Issue**: 连续播放时宿主位置会周期性跳至与轨道末关键帧一致的位置，造成镜头抖动；轨道方向在连续播放中不明显变化。
- **Debug Server**: Running on 127.0.0.1:7777
- **Log File**: .dbg/trae-debug-log-camera-position-rebound.ndjson

## Reproduction Steps
1. 加载含位置与方向差异明显的两个 Camera 关键帧的回放。
2. 从头连续播放至少五秒，观察镜头位置是否向后回弹及方向是否持续变化。
3. 中途暂停并恢复，观察首帧方向校正和后续状态。

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
| --- | --- | --- | --- | --- |
| A | 回放注入的宿主 MovePlayerPacket 在现有条件保护之外写回录制位置 | High | Low | Unresolved: 需记录 handler 命中与 runtime ID |
| B | LocalPlayer 的 normalTick 或物理状态在 frameUpdate 后重写旧位置 | Medium | Medium | Rejected: normalTick 入口前状态已跳至末点 |
| C | 快照或维度迁移路径在连续播放期间重复对宿主调用 moveTo/teleport | Low | Low | Rejected for sampled interval: pendingDimension=false，未见快照边界 |
| D | 轨道采样时间或 Camera 关键帧端点固定，导致连续方向目标本身不变 | Low | Low | Rejected: cameraTime 1→55，sample 连续递进且未到 lastKeyTick=84 |
| E | Actor 姿态已正确但独立的渲染相机插值仍使用旧位置/旋转 | High | Medium | Unresolved: 末点状态在 frameUpdate 与 normalTick 之间写入 |

## Log Evidence
- `cameraTime` 从 1 连续推进至 55，轨道样本从 `(49.217, 100.620, -34.287)` 连续变化至 `(33.930, 100.620, -22.758)`，从未越过末关键帧 84。
- 轨道目标正确时，实际位置仍多次在 `normalTick` 入口前变为固定 `(25.720, 100.620, -16.566)`；该点与轨道末关键帧空间位置一致。
- 该固定点在 `frameUpdate` 原始逻辑返回后也会出现，随后轨道 `moveTo` 能立刻把实际姿态恢复为当前样本。
- `normalTick` 前后位置不变，说明 normalTick 不是写入点；写入发生在上一帧轨道提交之后、下一次 normalTick 进入之前。

## Verification Conclusion
- 轨道采样与 yaw/pitch 映射正常；故障是另一个位置权威在帧间反复将宿主写至轨道末点。下一步应在回放注入 MovePlayer handler 和 Actor 位置写入边界记录调用数据，定位写入源后再做定向隔离。
