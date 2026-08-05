# 回放编辑器 Camera 轨道控制交接

更新时间：2026-08-05

## 一、当前目标与状态

### 1.1 目标

将回放中的本地宿主玩家 `mReplayPlayer` 作为无碰撞旁观者，在编辑器播放期间依据 Camera 关键帧轨道连续控制位置与朝向；暂停后释放控制，以便用户自由布置机位并创建关键帧。

### 1.2 当前状态

- P0 的大部分基础链路已实现：浮点 Camera 采样、能力接管/恢复、播放期间轨道姿态发布、snap/camera cut、录制移动包宿主隔离、暂停自由观察。
- 运行时仍存在未解决问题：连续播放时，宿主会周期性跳到一个固定位置，视觉表现为镜头抖动；方向随关键帧连续变化不明显。
- 已通过运行时日志确认：Camera 采样、回放时间推进、`moveTo` 的 `{pitch, yaw}` 参数顺序均正确。问题是另一条状态写入路径在帧间覆盖宿主姿态。
- 当前处于调试会话 `camera-position-rebound`，状态为 OPEN。不要在用户确认修复前清除调试代码、`.dbg` 日志、调试文档或调试服务。

## 二、已实施架构

### 2.1 数据与时间链路

```text
ReplaySession::getCameraTime()
  = currentTick + playbackTickAccumulator
  -> EditorController::applyPreviewCamera()
  -> CameraSampler::sampleAt(CameraEntity, double time)
  -> ReplaySession::setEditorCameraOverride(...)
  -> LocalPlayer::frameUpdate() 原生调用返回后
  -> ReplaySession::applyEditorCameraOverride()
  -> mReplayPlayer->moveTo(position, {pitch, yaw})
```

- 世界内容仍按整数 replay/source tick 回放；Camera 使用带小数 tick 的 `double` 时间。
- `CameraSampler` 纯函数完成位置、yaw、pitch、FOV 采样；`yaw/pitch` 的逻辑顺序为 `{yaw, pitch}`，引擎 `moveTo` 接收的 `Vec2` 顺序为 `{pitch, yaw}`。
- 初次接管会测量 `moveTo` 读回坐标与目标坐标的偏移，并在后续写入中抵消该偏移；历史观测到约 1.62 格的 Y 语义差异。

### 2.2 本地宿主控制

- 播放有效轨道时保存接管前能力，并启用 `NoClip`、`MayFly`、`Flying`。
- 暂停时清除轨道 pose，不再写入姿态，保留无碰撞能力以支持用户自由摆机位。
- 无 Camera、无样本、停止回放或离开回放世界时，清除 pose 并恢复接管前能力。
- seek、Camera 切换、Sequence 硬切、恢复播放首帧使用 `snap`，一次调用 `LocalPlayer::_forceCameraCut()`，连续帧不执行 cut。
- 当前最终姿态提交在 `LocalPlayer::frameUpdate` 的 `origin(context)` 返回后；接管期间短路 `localPlayerTurn` 与 `_applyTurnDelta`，防止鼠标旋转覆盖轨道。暂停和维度迁移会放行原生输入。

### 2.3 录制实体与宿主隔离

- `handleMoveEntities` 的精确/旧版路径在构造包前排除 `mReplayPlayer`。
- `mRecordedEntityIds` 不登记宿主，`clearRecordedEntities` 不删除宿主。
- `LegacyClientNetworkHandler::handle(MovePlayerPacket)` 已有 Hook：当包处于回放注入上下文、目标 runtime ID 是宿主、轨道接管有效、且不在维度迁移时，短路该包。
- 该保护尚未被证明覆盖当前固定点写入，下一轮调试必须记录该 Hook 的实际命中、runtime ID 和包位置。

### 2.4 已移除的控制源

- 已取消 `LevelRenderer::preRenderUpdate` 的安装和卸载。
- 旧 `CameraRenderOverride` 保留兼容空入口，但不再写 `mce::Camera`、ECS CameraComponent 或 CameraInstruction。
- 设计原则：运行时仅允许“原生自由移动”或“轨道接管”之一控制宿主，不能存在第二渲染相机写入源。

## 三、已验证事实

### 3.1 基础构建

- `xmake build playback`：最近一次通过。
- `xmake run refactor-model-tests`：最近一次通过。
- `git diff --check`：最近一次通过。
- 当前 `git status --short` 仅显示本交接文件为未跟踪文件；此前相机相关实现已存在于工作树/历史中。交接前应再次执行状态和差异检查。

### 3.2 运行时证据

本轮有效日志：`.dbg/trae-debug-log-camera-position-rebound.ndjson`。

1. **排除 Camera 采样到终点**
   - `cameraTime` 从 1 连续推进到至少 55；Camera 最后关键帧 tick 是 84。
   - 采样位置、yaw、pitch 持续变化，例如 tick 1 的样本 `(49.217, 100.620, -34.287, yaw=60.597, pitch=5.173)`，到 tick 55 为 `(33.930, 100.620, -22.758, yaw=46.441, pitch=9.825)`。
   - 结论：不是 `CameraSampler::sampleAt` 因时间越过末关键帧而固定返回终点。

2. **确认轨道写入正确**
   - 每次 `applyEditorCameraOverride()` 后，`mReplayPlayer` 的 position 与 rotation 都等于当前轨道样本。
   - 示例：日志 101–105，目标 `(44.687, 100.620, -30.871, yaw=56.402, pitch=6.551)`，写入后实际值一致。
   - 结论：`moveTo` 的位置及 `{pitch, yaw}` 映射正确，采样和写入链本身可用。

3. **确认固定位置覆盖存在**
   - 正确轨道目标期间，实际位置会多次变为固定 `(25.720, 100.620, -16.566)`，例如日志 20–21、99–100、128–129。
   - 该固定点被用户观察为轨道终点附近位置；它不是当时的当前 CameraSample。
   - 轨道随后立即将其写回当前样本，因此形成“当前轨道点 ↔ 固定点”的抖动。

4. **排除 normalTick 为直接写入点**
   - `LocalPlayer::normalTick` 前后实际 position 没有变化。
   - 固定位置在进入 `normalTick` 前已经出现，说明写入发生在“上一帧轨道 `moveTo` 结束”与“下一次 normalTick 入口”之间。

5. **当前仍未定位的写入源**
   - 回放普通 `GamePacket` 注入路径；可能存在未命中现有 `MovePlayerPacket` 守卫的情况。
   - `LocalPlayer` 或独立渲染相机在 frame/tick 交界处提交的历史插值状态。
   - 当前项目未显式记录其它 Actor/LocalPlayer 位置 API 对宿主的调用来源。

## 四、进行中的调试

### 4.1 调试会话与文件

- 主调试文档：[debug-camera-position-rebound.md](file:///d:/raplay/Playback/debug-camera-position-rebound.md)
- 前一轮调试记录：[debug-camera-spectator-jitter.md](file:///d:/raplay/Playback/debug-camera-spectator-jitter.md)
- 当前日志：[trae-debug-log-camera-position-rebound.ndjson](file:///d:/raplay/Playback/.dbg/trae-debug-log-camera-position-rebound.ndjson)
- 调试服务命令：

```powershell
python "c:\Users\Administrator\.trae-cn\builtin_skills\TRAE-debugger\tools\debug-server\python\debug-server.py" --session camera-position-rebound --outdir .dbg --clean --idle 1200
```

- 服务按最近状态监听 `127.0.0.1:7777`；注意它会因 `--idle 1200` 空闲超时退出。重新让用户复现前先确认服务仍在运行并按需重启/清空日志。

### 4.2 已加入的临时诊断

- `ReplaySession::debugReportCameraSample(...)`：记录 Camera ID、`cameraTime`、首尾关键帧 tick、发布 position/yaw/pitch。
- `ReplaySession::debugReportCameraState(...)`：仅在回放已激活、已进入回放世界、未暂停、轨道 active 且对象是宿主时记录实际姿态。
- 采样点：`LocalPlayer::normalTick` 前后、`LocalPlayer::frameUpdate` 前后、`applyEditorCameraOverride` 的 `moveTo` 前后。
- 上报以 detached 后台线程执行 WinHTTP，避免之前在主线程同步 HTTP 导致播放速度降为约 0.1x 的性能回归。
- 临时依赖：`xmake.lua` 增加系统库 `winhttp`；诊断实现位于 `ReplaySession.cpp`，使用 `windows.h` 和 `winhttp.h`。

### 4.3 调试注意事项

- 后台线程上报会造成日志时间戳/行顺序轻微乱序；判断因果时优先使用 `tick`、`location` 和 position 数据，不要只按 NDJSON 的文件行序推断调用顺序。
- `debugReportCameraState` 有全局采样上限 120，`debugReportCameraSample` 有上限 60；日志采样必须只在有效播放期开始，避免在 paused/initialization 阶段耗尽配额。
- 不能再在主线程中执行 WinHTTP；此前已证实会严重降低客户端 update 和回放 tick 吞吐。

## 五、后续交接步骤

### 5.1 第一优先级：定位固定点的写入边界

只增加诊断，不改业务逻辑。建议按下列顺序：

1. 在 `PlaybackReplayHostMovePlayerHook` 中记录每一个 `MovePlayerPacket`：
   - `isInjectingPacket(&packet)`；
   - packet runtime ID；
   - 是否等于 `mReplayPlayer` runtime ID；
   - `shouldRejectReplayHostMove()` 的结果；
   - packet position、rotation、mode；
   - 是否调用 `origin`。
2. 为 `Actor::moveTo(Vec3, Vec2)` 安装临时 Hook，只在对象指针等于 `ReplaySession::getReplayPlayer()` 时记录：
   - 调用目标 position/rotation；
   - 是否处于回放注入上下文；
   - 当前 `cameraTime/currentTick`；
   - 调用发生时宿主实际 position。
3. 如固定点到达时没有任何宿主 `MovePlayerPacket` 或 `Actor::moveTo` 调用，则进一步观测 `LocalPlayer::teleportTo(...)`、`Actor::setPos(...)` 或引擎帧相机的独立插值状态；不可直接假设替代 API 安全可用。

### 5.2 分支处理策略

| 证据 | 最小修复方向 |
| --- | --- |
| 回放注入宿主 `MovePlayerPacket` 的位置等于固定点，且 Hook 未拒绝 | 将保护从“轨道 active/非维度迁移”改为严格的“回放注入 + 宿主身份”拒绝；维度传送用带 transition generation、目标维度和预期位置的明确白名单，不能笼统放行。 |
| 存在项目内其它 `moveTo` 写宿主固定点 | 删除/条件化该写入；快照和维度路径仅在 seek、forced snapshot 或明确维度迁移时允许写宿主，并在恢复轨道时标记 snap。 |
| 未发现包或 `moveTo`，但 LocalPlayer 状态仍变为固定点 | 继续探索 `LocalPlayer` 的历史插值/渲染相机状态；不要粗暴禁用整个 `normalTick`、`aiStep` 或 `frameUpdate`，否则会破坏维度、区块、输入与回放生命周期。 |
| Actor 实际姿态始终正确但画面仍抖动 | 说明 `mce::Camera` 或原生渲染相机有独立状态；需要只读定位该状态，再设计受控的渲染相机接管，不应恢复旧的多写源 `CameraRenderOverride`。 |

### 5.3 完成条件

- 连续播放两关键帧直线和贝塞尔轨道：位置、yaw、pitch 连续，无固定点瞬移或抖动。
- 播放时鼠标不能覆盖轨道方向；暂停后立即恢复自由移动和自由观察。
- seek、Camera 切换、Sequence 硬切、跨维度：仅首帧 snap，无残留插值。
- 录制主角继续正常移动，宿主不会被录制包驱动。
- 停止回放后能力、游戏模式和控制状态均恢复。
- 修复获得用户明确确认后，清理本轮临时诊断、`winhttp` 依赖、调试服务、`.dbg` 日志和 `debug-camera-position-rebound.md`；保留并更新正式模块文档。

## 六、相关文件索引

| 文件 | 作用 |
| --- | --- |
| [camera-spectator-control.md](file:///d:/raplay/Playback/docs/editor/camera-spectator-control.md) | 正式模块文档，包含需求、架构、已落地实现、验证矩阵与不变量。 |
| [ReplaySession.h](file:///d:/raplay/Playback/src/playback/functions/replay/ReplaySession.h) | 轨道状态、能力备份、会话 API、当前临时调试声明。 |
| [ReplaySession.cpp](file:///d:/raplay/Playback/src/playback/functions/replay/ReplaySession.cpp) | 轨道 pose 写入、快照、回放包注入、维度迁移、临时调试上报。 |
| [ClientTickHooks.cpp](file:///d:/raplay/Playback/src/playback/functions/tick/ClientTickHooks.cpp) | `_subTick` 后发布轨道、`frameUpdate` 后提交姿态、normalTick 观测、输入旋转隔离。 |
| [NetworkHooks.cpp](file:///d:/raplay/Playback/src/playback/functions/record/NetworkHooks.cpp) | `MovePlayerPacket` 最终边界守卫；下一步首要诊断位置。 |
| [EditorController.cpp](file:///d:/raplay/Playback/src/playback/editor/controller/EditorController.cpp) | 选择/解析 Camera、按 `getCameraTime()` 采样、发布 pose。 |
| [CameraSampler.cpp](file:///d:/raplay/Playback/src/playback/refactor/camera-motion/CameraSampler.cpp) | `double` tick CameraSample 纯函数；已由日志证明采样连续正确。 |
| [ModelTests.cpp](file:///d:/raplay/Playback/tests/refactor/models/ModelTests.cpp) | CameraSampler 浮点 tick 模型测试。 |
| [debug-camera-spectator-jitter.md](file:///d:/raplay/Playback/debug-camera-spectator-jitter.md) | 前几轮抖动/性能回归/输入隔离的历史证据。 |
| [debug-camera-position-rebound.md](file:///d:/raplay/Playback/debug-camera-position-rebound.md) | 当前调试会话、假设和已确认/排除项。 |

## 七、操作约束

- 用户规则：任何模块功能开发必须有专属文档，且包含“需求、架构、执行”三部分；正式文档已是 `docs/editor/camera-spectator-control.md`，后续新增行为需同步更新。
- 用户偏好：尽量使用 codegraph 进行代码探索。
- 修改业务逻辑前必须先取得下一轮运行时证据；当前会话处于证据收集阶段。
- 不要提交 Git 变更，除非用户明确要求提交。
- 不要新增与需求无关的代码注释；调试点采用调试技能要求的 `#region debug-point ...` 区域是当前允许保留的临时诊断标识。

---

## 八、内置静态 FFmpeg 构建交接（2026-08-05）

> 本节与 Camera 轨道控制调试相互独立，属于导出链路的底层依赖。两件事同时处于交接状态，完成条件互不阻塞。

### 8.1 需求

Playback 导出链路以 **GPL 静态链接**方式内置 FFmpeg 8.1.2（libav API），不依赖外部 `ffmpeg.exe` 或用户手动配置。GPL 合规要求随发行物提供许可证、对应源码、精确 configure 参数（见 manifest）。

### 8.2 已交付架构

- **工具链**：MSVC x64（VS 18 BuildTools，`vcvars64.bat`），通过 MSYS2 bash 执行构建；全部库统一 `/MD`（动态 CRT）。
- **配置**：`--enable-static --disable-shared`，启用 GPL 软编（libx264/libx265/libvpx/libopus）+ zlib；含 avcodec/avformat/avutil/swscale/swresample/avfilter。
- **编码器**：aac/alac/flac/gif/libopus/libvpx_vp8/libvpx_vp9/libx264/libx265/mjpeg/mp3/png/prores/rawvideo/vorbis/webp 等 20 个。
- **封装器/解封装器**：mp4/mov/matroska/webm/wav/gif/apng/avi/ogg/image2/flac/webp 等 14 个。
- **数据链路**：`libav` 静态库 → `LibavEncoder`/`LibavMuxer` 调用 → 导出管线（`docs/refactor/11-export-pipeline.md`）。

### 8.3 产物位置与"直接可用"拷贝

| 路径 | 内容 | 是否入库 |
|---|---|---|
| `third_party/ffmpeg/` | 权威产物：`include/` + `lib/`（14 个 .lib）+ `ffmpeg-build-manifest.json` | 否（.gitignore） |
| `third_party/ffmpeg-src/` | 各库源码与 tarball（审计/对应源码用） | 否 |
| `lib/` | **本次新增的可分发拷贝**：14 个 `.lib` + `include/` 全套头文件 | `.lib` 被 .gitignore 忽略，头文件可入库 |
| `licenses/ffmpeg/` | 各库许可证文本 | 是 |
| `scripts/build-ffmpeg/` | 构建脚本 + 版本清单 + README + smoke 测试 | 是 |

**换机器无需重建**：`lib/` 即"能直接用的文件"，拷到任何 Windows x64 机器用 MSVC 链接即可（静态库不依赖构建环境）。约束：

- 链接机需 MSVC x64 工具链（VS2019+，库为 `/MD` 动态 CRT）；
- 运行机需 VC++ Redistributable（Windows 10+ 自带 UCRT，vcruntime140/msvcp140 需随应用分发）；
- `lib/` 的 `.lib` 被 [.gitignore](file:///d:/raplay/Playback/.gitignore) 第 10/42 行忽略，要随版本分发需显式加白名单或归档。

### 8.4 使用与验证

```powershell
# 接入导出链路（当前仍未执行，属待办）
xmake f --playback_ffmpeg=y
xmake build playback

# 独立验证静态链接链路（已通过：RESULT: ALL OK）
powershell -ExecutionPolicy Bypass -File scripts/build-ffmpeg/smoke-libav.ps1
```

- smoke 覆盖：20 个编码器/7 个解码器/6 个封装器/4 个解封装器注册、swscale、swresample、libx264→mp4→h264 真实往返。
- 若从 `lib/` 链接，把 xmake 的 `setup_ffmpeg` 的 includedirs/linkdirs 从 `third_party/ffmpeg` 改为 `lib` 即可（当前未改，仍指向 `third_party/ffmpeg`）。

### 8.5 重建（仅当产物丢失或升级版本时）

前置：MSYS2（`pacman -S --needed git make nasm pkg-config cmake diffutils`）+ VS2022 桌面/C++ 工作负载。然后一条命令幂等重建：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-ffmpeg/build-ffmpeg.ps1
```

版本清单在 `scripts/build-ffmpeg/versions.txt`（FFmpeg 8.1.2、x264 commit、x265 3.6、libvpx v1.15.0、libopus v1.5.2、zlib v1.3.1）；全部已踩坑（pkgconf 签名、CRT 统一 `-MD`、zconf.h `HAVE_UNISTD_H` 陷阱、zlib 静态化、vpx.pc 去 `-lm`）已固化进脚本与 `scripts/build-ffmpeg/README.md` 故障排查表。

### 8.6 待办

1. `xmake f --playback_ffmpeg=y && xmake build playback` 接入导出链路，验证 `LibavEncoder`/`LibavMuxer` 实际调用；
2. 更新 `THIRD_PARTY_NOTICES.md`（GPL 静态链接声明 + 各库许可证）；
3. 发布前固定 x264 commit（manifest 已记录当前 HEAD `0480cb05...`）；
4. 决定 `lib/` 产物分发方式（git 白名单 or 发布归档），当前 `.lib` 不入库；
5. 与相机调试共用 `.dbg` 目录时注意：FFmpeg 相关日志/临时文件已清理，保留 `smoke_libav.exe` 供后续链接调试。
