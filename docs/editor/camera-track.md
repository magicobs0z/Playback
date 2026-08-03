# editor/camera-track — 摄像机实体与自由机位关键帧

> 入口：`src/playback/editor/editing/models/CameraEntity.h`、`src/playback/editor/editing/commands/CameraCommands.*`。
> 角色：定义可被摄像机序列段绑定的 `CameraEntity`，并以“移动自由相机到机位 → 在当前时间轴创建关键帧”的方式完成镜头动画创作。
> 工作流权威：[refactor/09-video-editing-workflow.md](file:///d:/raplay/Playback/docs/refactor/09-video-editing-workflow.md)。本文细化其中 Camera 条目的创作、采样、预览和持久化规则。

## 一、需求（Requirements）

### 1.1 功能性需求

| ID | 需求 | 优先级 |
|---|---|---|
| CT-1 | 摄像机是 `CameraEntity`，可由用户新建自由摄影机，或由子 Actor 创建绑定摄影机；不再存在独立 `CameraTrack` 资产 | P0 |
| CT-2 | `CameraEntity.kind=Keyframe` 是默认且完整支持的创作模式；每台 Camera 在时间轴上对应一行 | P0 |
| CT-3 | 用户移动自由相机到世界中的目标机位后，可在当前编辑时间轴 tick 创建关键帧 | P0 |
| CT-4 | 创建关键帧必须捕获自由相机的完整状态：世界位置、yaw/pitch、垂直 FOV | P0 |
| CT-5 | 创建关键帧后自由相机保持可移动；未再次创建关键帧前的移动仅作为临时预览状态 | P0 |
| CT-6 | 时间轴跳转时，当前编辑的 Keyframe Camera 自动同步到该 Camera 在当前编辑时间轴 tick 的采样状态 | P0 |
| CT-7 | 关键帧支持创建或覆盖、删除、拖拽移动、吸附到 tick、字段编辑及 Undo/Redo | P0 |
| CT-8 | 支持 Linear、EaseIn、EaseOut、EaseInOut、CubicBezier 五种插值缓动 | P0 |
| CT-9 | 预览与导出均能按编辑时间轴 tick 确定性采样 Camera 的位置、旋转和 FOV | P0 |
| CT-10 | 摄像机序列段通过 `cameraId` 绑定 Camera，并在段边界硬切；序列段不保存关键帧 | P0 |
| CT-11 | Camera、关键帧和绑定关系持久化到 `.playback` 的 `PlaybackMeta.editor` 节点 | P0 |
| CT-12 | 保留 Path、Rig、Preset 作为后续 Camera kind 扩展；首版不得削弱 Keyframe 主创作流程 | P1 |

### 1.2 非功能性需求

- 单台 Camera 最多 1024 个关键帧；关键帧按 tick 严格递增。
- 在 1024 帧以内使用二分定位；采样无锁、纯函数，目标耗时小于 0.1ms。
- 相同 Camera 和相同编辑时间轴 tick 多次采样结果一致，浮点误差不超过 `1e-5`。
- 预览、导出、时间轴跳转必须使用同一采样实现。
- Undo/Redo 栈最多保留 100 个命令。

### 1.3 与工作流的约束对齐

- 时间轴仅有固定的 Camera Sequence、World Actor 与 0..N 个 Camera 行；详细定义见 [09-video-editing-workflow.md §2.1](file:///d:/raplay/Playback/docs/refactor/09-video-editing-workflow.md#L52-L79)。
- `timelineTick` 是 Camera 关键帧唯一的时间坐标，范围为 `[0, EditorStateExt.totalTicks]`。
- `sourceTick` 只能由 World Actor 片段映射得出，用于定位回放世界；不得用于 Camera 的关键帧采样。
- 摄像机序列负责镜头选择与硬切，World Actor 是编辑时间轴到回放源 tick 的唯一映射器。
- 编辑状态、命令与 UI 复用 `EditorStateExt`、`CommandStack` 和现有 ImGui 编辑器骨架。

## 二、架构（Architecture）

### 2.1 职责边界

```
自由相机输入 / Viewport
        │ 捕获完整状态
        ▼
CaptureCameraKeyframeCommand
        │ execute / undo
        ▼
EditorStateExt.cameras[] ─────► TimelinePanel / DetailsPanel
        │                              │
        │ CameraEntity + timelineTick  │ 编辑、拖拽、缓动
        ▼                              ▼
CameraSystem::sampleAt ◄──── CommandStack
        │
        ├──► Viewport：同步自由相机并实时预览
        └──► RenderJob：应用到 MCBE 后抓帧

WorldActorOps::mapTimelineToSourceTick
        └──► ReplaySession：仅负责回放世界状态
```

| 单元 | 职责 | 不负责 |
|---|---|---|
| `CameraEntity` | 保存一台摄影机的标识、类型、关键帧和绑定信息 | 不决定何时使用该摄影机 |
| `CameraKeyframe` | 保存一个编辑时间点的完整机位和离开该帧的缓动 | 不保存世界回放 tick |
| `CameraCommands` | 以命令方式修改 Camera 与关键帧并提供 Undo/Redo | 不直接写 MCBE CameraManager |
| `CameraSystem` | 纯函数采样与将采样结果应用至隔离回放世界 | 不映射 World Actor 时间 |
| `SequenceSegment` | 在自身时间范围内选择绑定的 `cameraId` | 不保存或重映射 Camera 动画 |
| `WorldActorOps` | 将 `timelineTick` 映射到 `sourceTick` | 不干预 Camera 参数 |

### 2.2 数据模型

```cpp
enum class EasingType : uint8_t {
    Linear = 0,
    EaseIn,
    EaseOut,
    EaseInOut,
    CubicBezier
};

enum class CameraPathType : uint8_t {
    Linear = 0,
    CubicBezier,
    AutoSmooth
};

enum class CameraTransitionPreset : uint8_t {
    Custom = 0,
    LinearConstant,
    CinematicEase,
    ArcPushIn,
    ArcPullOut,
    OrbitPass,
    WhipPan,
    ZoomTransition
};

struct CameraMotionSegment {
    CameraPathType pathType{CameraPathType::Linear};
    CameraTransitionPreset preset{CameraTransitionPreset::LinearConstant};
    Vec3 outControl{};
    Vec3 inControl{};
    bool useLookAlongPath{};
    float fovPeakOffset{};
};

struct CameraKeyframe {
    std::string id;
    int tick{};
    Vec3 position{};
    Vec2 rotation{};
    float fov{90.0f};
    EasingType easingType{EasingType::Linear};
    Vec2 bezierCtrl1{0.42f, 0.0f};
    Vec2 bezierCtrl2{0.58f, 1.0f};
    CameraMotionSegment outgoingMotion{};
};

struct CameraEntity {
    std::string id;
    std::string name;
    CameraKind kind{CameraKind::Keyframe};
    std::vector<CameraKeyframe> keys;
    std::optional<CameraPath> path;
    std::optional<CameraRig> rig;
    std::optional<CameraPreset> preset;
    std::optional<CameraShake> shake;
    std::optional<CameraLimiter> limiter;
    std::string bindingEntityUuid;
    int bindingMode{};
    float bindingDamping{0.1f};
    bool active{};
    bool locked{};
};

struct CameraSample {
    Vec3 position{};
    Vec2 rotation{};
    float fov{90.0f};
    bool valid{};
};
```

**数据规则：**

- `CameraEntity.id` 是唯一稳定标识；`SequenceSegment.cameraId` 只引用该 id，不引用可变的名称或数组下标。
- `keys` 以 `tick` 升序保存，任意两个关键帧不得有相同 tick。
- 同一 tick 再次创建关键帧时覆盖既有帧的完整机位，不增加关键帧数量。
- `easingType`、时间贝塞尔控制点和 `outgoingMotion` 均属于区间起点，控制当前帧到下一帧；最后一帧保留字段但不参与区间计算。
- 贝塞尔控制点的 x 坐标钳制至 `[0, 1]`，y 坐标允许超出该范围以支持过冲。
- `outControl`、`inControl` 是 3D 三次贝塞尔的两个控制点，分别相对区间起点与终点的世界位置保存；`Linear` 忽略它们，`AutoSmooth` 由相邻关键帧自动计算，不持久化推导结果。
- `fovPeakOffset` 是区间中点的临时 FOV 偏移，用于变焦类预设；起止帧的 FOV 永远以用户捕获或编辑的值为准。
- `CameraKind::Keyframe` 使用 `keys`；其他 kind 的扩展字段不得改变 Keyframe 的时间语义。

### 2.3 自由机位创作流程

```
选中或新建 Keyframe Camera
        │
        ▼
将 playhead 移到目标 timelineTick
        │
        ▼
Viewport 同步该 Camera 在此 tick 的采样状态
        │
        ▼
用户移动 / 旋转 / 缩放自由相机
        │ 临时预览，不修改 keys
        ▼
点击“创建关键帧”
        │
        ▼
CaptureCameraKeyframe(cameraId, timelineTick, freeCameraState)
        │
        ├── 无同 tick 帧：插入并保持排序
        └── 有同 tick 帧：完整覆盖 position / rotation / fov
```

新建自由摄影机时，系统在当前 playhead 自动创建第一帧，使该 Camera 可以立即绑定到序列段并参与预览。创建或覆盖后不锁定自由相机，用户可以继续移动到下一机位；只有下一次明确创建关键帧才会持久化新状态。

若选中 Camera 自身，则 Viewport 直接预览该 Camera。若选中 Camera Sequence，则先解析当前 SequenceSegment 的 Camera，再预览该 Camera。未绑定段回退到 `cameras[0]`；Camera 列表为空时显示“无摄像机可预览/导出”的可恢复错误。

### 2.4 命令与 UI

| 操作 | 命令 | 结果 |
|---|---|---|
| 新建自由摄影机 | `AddFreeCamera` | 新增 `CameraEntity`，并在当前 tick 创建首个完整关键帧 |
| 创建或覆盖关键帧 | `CaptureCameraKeyframe` | 从自由相机捕获完整状态，按 tick 插入或覆盖 |
| 移动关键帧 | `MoveKeyframe` | 修改 tick，吸附并拒绝与其他帧重复 |
| 删除关键帧 | `DeleteKeyframe` | 删除目标帧；允许 Keyframe Camera 暂时为空 |
| 改关键帧字段 | `SetCameraKeyframeState` | 修改 position、rotation 或 fov |
| 改缓动 | `SetKeyframeEasing` | 修改区间起点的 easing 与控制点 |
| 创建绑定摄影机 | `CreateBindingCamera` | 新增绑定指定子 Actor 的 Camera 实体 |
| 删除摄影机 | `DeleteCamera` | 删除 Camera 并把所有引用它的序列段 `cameraId` 清空 |

每项操作必须是 `CommandStack` 中可逆命令。拖拽关键帧期间可本地预览，鼠标释放时只提交一个 `MoveKeyframe`，避免 Undo 栈因连续输入膨胀。

Timeline 的 Camera 行绘制关键帧菱形；当前帧和已选帧使用高亮。Details 面板根据选择展示：

| 选择项 | 内容 |
|---|---|
| Camera | 名称、kind、绑定信息、锁定状态、关键帧列表、创建关键帧按钮 |
| Keyframe | tick、position、rotation、fov、easing、CubicBezier 控制点 |
| SequenceSegment | 绑定 Camera 的下拉列表与未绑定警告 |

### 2.5 确定性采样

```cpp
CameraSample CameraSystem::sampleAt(
    const CameraEntity& camera,
    int timelineTick,
    const ReplaySession& session);
```

对 `CameraKind::Keyframe` 的处理：

1. `keys` 为空，返回 `valid=false`。
2. `timelineTick` 小于等于首帧 tick，返回首帧完整状态。
3. `timelineTick` 大于等于末帧 tick，返回末帧完整状态。
4. 使用 `std::upper_bound` 在 `O(log n)` 内定位相邻帧 `[A, B]`，计算 `t=(timelineTick-A.tick)/float(B.tick-A.tick)`。
5. 按 A 的 easing 将 `t` 转为 `easedT`；position、pitch、fov 使用 `easedT` 插值，yaw 按最短角路径插值。

| Easing | `easedT` |
|---|---|
| Linear | `t` |
| EaseIn | `t * t` |
| EaseOut | `1 - (1 - t) * (1 - t)` |
| EaseInOut | `t < 0.5 ? 2*t*t : 1 - pow(-2*t+2, 2)/2` |
| CubicBezier | 对 `(0,0) → ctrl1 → ctrl2 → (1,1)` 求 `x=t` 的固定次数反解，再取对应 y |

贝塞尔反解使用固定次数的 Newton-Raphson 迭代，并在导数接近零时退回二分区间，避免平台或输入差异导致非确定结果。所有中间结果使用 `float`，不读取时钟、随机数或全局可变状态。

### 2.6 预览与导出时间流

```cpp
const int timelineTick = currentTimelineTick;
const SequenceSegment* segment = findSegmentAt(editorState.sequence, timelineTick);
const CameraEntity* camera = resolveCamera(editorState.cameras, segment->cameraId);
const int sourceTick = WorldActorOps::mapTimelineToSourceTick(
    editorState.worldActor, timelineTick);

replaySession.requestSeek(sourceTick);
replaySession.tick();

const CameraSample sample = CameraSystem::sampleAt(
    *camera, timelineTick, replaySession);
CameraSystem::applyToMCBE(sample);
```

必须严格区分两套时间：

| 时间 | 用途 | 产生者 |
|---|---|---|
| `timelineTick` | 查找 SequenceSegment、采样 Camera、编辑关键帧 | 编辑器时间轴 |
| `sourceTick` | seek / tick ReplaySession，决定世界画面内容 | `WorldActorOps` |

因此改变 World Actor 片段的播放速度会改变同一编辑时间点的世界内容，但不会改变同一编辑时间点的 Camera 位置、旋转或 FOV。Camera Sequence 发生切段时直接改用新段绑定 Camera 的采样结果，不对两台 Camera 作过渡混合。

`applyToMCBE` 仅能在 `__playback_replay_world__` 的隔离编辑/渲染模式下执行；非隔离世界必须拒绝外部摄影机覆盖，避免影响在线世界或触发服务端校验。

### 2.7 持久化与迁移

`.playback` ZIP 的 `metadata.json` 中，所有编辑数据位于 `PlaybackMeta.editor`。Camera 使用如下结构持久化：

```json
{
  "editor": {
    "version": 3,
    "cameras": [
      {
        "id": "camera-main",
        "name": "Main",
        "kind": 0,
        "keys": [
          {
            "id": "camera-main.key.0",
            "tick": 0,
            "px": 0.0,
            "py": 80.0,
            "pz": 0.0,
            "yaw": 0.0,
            "pitch": 0.0,
            "fov": 90.0,
            "easing": 0,
            "c1x": 0.42,
            "c1y": 0.0,
            "c2x": 0.58,
            "c2y": 1.0
          }
        ]
      }
    ]
  }
}
```

- 新存档只写 `editor.cameras`、`editor.sequence`、`editor.worldActor` 等 v3 字段，不写旧 `cameraTracks`、`videoTracks`、`activeCameraTrackIdx`。
- v1/v2 或旧 `cameraTracks` 存档加载时，将每条旧 CameraTrack 迁移为 `CameraKind::Keyframe` 的 `CameraEntity`，完整保留关键帧；旧 Clip 的活动轨道索引尽力迁移为对应序列段 `cameraId`。
- 没有 `editor` 节点的历史回放重建默认 Sequence 与 WorldActor，并创建零台 Camera；不会自动创建虚假的默认摄影机。
- 未来未知版本拒绝加载并提示升级，不能以不明数据继续导出。

## 三、执行（Execution）

### 3.1 实现顺序

| 步骤 | 文件 / 模块 | 内容 | 验证 |
|---|---|---|---|
| 1 | `editing/models/CameraKeyframe.h` | 收敛完整关键帧字段、两个贝塞尔控制点与序列化 | 单测：JSON round-trip |
| 2 | `editing/models/CameraEntity.h` | 以 `CameraEntity` 作为唯一 Camera 资产，清理旧 CameraTrack 依赖 | 编译 |
| 3 | `editing/CameraSystem.*` | 实现以 `timelineTick` 为输入的确定性 Keyframe 采样 | 单测：边界、五种 easing、yaw 最短路径 |
| 4 | `editing/commands/CameraCommands.*` | 增加捕获/覆盖完整自由机位的命令，完善 CRUD 与 Undo/Redo | 单测：execute/undo 互逆 |
| 5 | `editing/models/EditorStateExt.h` | 移除 `cameraTracks`、旧 video 轨和活动轨索引语义 | 编译 + 迁移测试 |
| 6 | `ui/panels/TimelinePanel.*` | Camera 行绘制关键帧、选择、拖拽和吸附 | 手动：一次拖拽只生成一个 Undo 项 |
| 7 | `ui/panels/DetailsPanel.*` | Camera / Keyframe / SequenceSegment 上下文编辑 | 手动：字段保存和撤销 |
| 8 | `ViewportPanel` / `RealtimePreview` | 时间跳转同步采样相机，移动仅作为临时预览 | 手动：连续布置机位 |
| 9 | `RenderJob` | 按 `timelineTick` 采样 Camera，按 `sourceTick` 推进世界 | 导出：世界变速不改变镜头节奏 |
| 10 | 编辑数据序列化与迁移 | 保存 v3 数据，迁移旧 `cameraTracks` | 单测：旧档迁移和 v3 round-trip |

### 3.2 关键不变量

1. **Camera 是唯一资产**：`CameraEntity` 替代独立 `CameraTrack` 与 `CameraTrackExt`；序列只保存 `cameraId`。
2. **关键帧使用编辑时间**：所有 Camera key 的 tick 都是 `timelineTick`，绝不写入 `sourceTick`。
3. **同 tick 唯一**：创建重复 tick 时完整覆盖，不产生零长度插值区间。
4. **自由机位显式落帧**：移动自由相机不改持久化状态；只有命令捕获时才创建或更新关键帧。
5. **采样纯函数**：采样只依赖 Camera、`timelineTick` 和必要的只读绑定信息；不修改编辑状态。
6. **时间职责分离**：World Actor 是 `timelineTick → sourceTick` 的唯一映射器；CameraSystem 不参与该映射。
7. **序列硬切**：切段只切 Camera 选择，不混合不同 Camera 的参数。
8. **安全隔离**：只有本地回放世界可以接收 MCBE 摄影机覆盖。

### 3.3 测试用例

| ID | 用例 | 期望 |
|---|---|---|
| CT-T1 | 新建自由摄影机 | 在当前 `timelineTick` 有一个包含完整自由机位状态的首帧 |
| CT-T2 | 移动自由相机但不创建帧 | Camera 的持久化 `keys` 不变，Viewport 显示临时状态 |
| CT-T3 | 同 tick 两次创建帧 | 关键帧数量不变，position / rotation / fov 完整覆盖 |
| CT-T4 | 两帧 Linear 中点 | position、pitch、fov 为严格中点，yaw 走最短角路径 |
| CT-T5 | 各 easing 的端点 | `t=0` 返回 A，`t=1` 返回 B；贝塞尔端点精确对齐 |
| CT-T6 | 1024 帧随机采样 | 二分定位正确，结果可重复，单次采样满足性能目标 |
| CT-T7 | World Actor 变速 | 相同 `timelineTick` 的 CameraSample 不变，`sourceTick` 改变 |
| CT-T8 | Sequence 切换 Camera | 段边界硬切到新 Camera，无混合 |
| CT-T9 | 删除 Camera | Camera 被删除，所有引用段 `cameraId` 清空且可 Undo |
| CT-T10 | 旧 `cameraTracks` 加载 | 每条旧轨迁移为一台 Keyframe Camera，关键帧数据不丢失 |
| CT-T11 | v3 保存后重读 | `cameras`、关键帧、序列绑定和缓动控制点一致 |
| CT-T12 | 非隔离世界调用 apply | 拒绝覆盖摄影机且不修改游戏相机 |

### 3.4 风险与回退

| 风险 | 缓解 |
|---|---|
| 用户误以为移动自由相机会自动保存 | Viewport 明确显示“未落帧”临时状态；创建关键帧按钮保持可见 |
| 世界变速被误用于 Camera 采样 | `CameraSystem::sampleAt` 参数命名为 `timelineTick`，并以 CT-T7 回归测试保护 |
| 删除 Camera 导致镜头段无绑定 | 清空 `cameraId`，运行时回退第一台 Camera；无 Camera 时显示可恢复错误 |
| 旧模型字段与新模型并存 | 仅加载期迁移，v3 保存不再写旧字段，并在状态模型中删除旧字段 |
| 贝塞尔控制点造成求解不稳定 | x 坐标钳制、固定迭代次数、导数退化时二分回退 |

## 四、模块关系

### 被谁调用（上游）

- `TimelinePanel`：显示 Camera 行与关键帧，并发起选择、拖拽、创建和删除操作。
- `DetailsPanel`：编辑 Camera、关键帧和 SequenceSegment 的属性。
- `ViewportPanel` / `RealtimePreview`：在编辑时间轴跳转时同步相机，在移动时提供临时机位预览。
- `RenderJob`：沿摄像机序列解析 Camera，并在导出帧的 `timelineTick` 调用采样。
- `CommandStack`：执行所有 Camera 命令并维护 Undo/Redo。

### 调用谁（下游）

- `EditorStateExt.cameras`：保存 Camera 实体与关键帧。
- `SequenceSegment.cameraId`：确定当前序列段绑定的 Camera。
- `WorldActorOps::mapTimelineToSourceTick`：只用于取得回放世界的 `sourceTick`。
- `ReplaySession`：推进隔离回放世界。
- `CameraSystem::applyToMCBE`：将采样结果安全应用到回放世界的相机。

### 共享数据

- `EditorStateExt.cameras`：Timeline、Details、Viewport、RenderJob 的摄影机资产来源。
- `EditorStateExt.currentTick`：创建、编辑和采样关键帧的 `timelineTick`。
- `EditorStateExt.sequence`：决定该 tick 应使用哪台 Camera。
- `EditorStateExt.worldActor`：决定该 tick 对应哪个 `sourceTick`，与 Camera 关键帧时间独立。

## 五、阅读顺序

1. 本文：CameraEntity 与自由机位关键帧创作。
2. [09-video-editing-workflow.md](file:///d:/raplay/Playback/docs/refactor/09-video-editing-workflow.md)：3 条一级轨道、Sequence 与 World Actor 的总工作流。
3. [02-camera-motion.md](file:///d:/raplay/Playback/docs/refactor/02-camera-motion.md)：CameraSystem 的其他运动类型与算法扩展。
4. [05-render-pipeline.md](file:///d:/raplay/Playback/docs/refactor/05-render-pipeline.md)：导出帧与回放世界的渲染管线。
5. [06-data-persistence.md](file:///d:/raplay/Playback/docs/refactor/06-data-persistence.md)：编辑器数据版本与持久化规则。
