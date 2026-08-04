# editor/camera-render-override — 摄像机渲染帧覆盖（已废弃）

> 入口：`src/playback/editor/camera-render/`
> 状态：已废弃，不再作为 Camera 轨道预览实现。
> 替代设计：[camera-spectator-control.md](file:///d:/raplay/Playback/docs/editor/camera-spectator-control.md)。
> 原因：`CameraComponent` 与 `mce::Camera` 在当前 MCBE 版本不是可稳定控制最终画面的入口；最终实现改由本地无碰撞旁观者承载镜头。

## 一、需求

### 1.1 功能需求

| ID | 需求 | 优先级 |
|---|---|---|
| CRO-1 | 已选中 Camera 或当前 Camera Sequence 段绑定 Camera 时，预览按 `timelineTick` 采样位置与 yaw/pitch | P0 |
| CRO-2 | 采样结果仅在渲染帧阶段覆盖真实渲染 CameraComponent，不修改回放 Player | P0 |
| CRO-3 | 覆盖启用时不使用 CameraInstructionComponent，不触发实体物理、碰撞或网络同步 | P0 |
| CRO-4 | 清除预览、无有效采样、离开回放世界或停止回放时，立即停止覆盖并恢复原生相机 | P0 |
| CRO-5 | 覆盖状态保留 FOV 字段和 `applyFov` 扩展点，但首版不修改投影/FOV | P1 |

### 1.2 验收标准

- 两个不同位置和朝向的关键帧之间，Viewport 画面的位置和朝向按 CameraSampler 的结果连续变化。
- 预览期间不再出现回放玩家碰撞高度造成的上下抖动。
- 不新增 CameraInstruction 调用、玩家移动或同步网络请求。
- 取消 Camera 预览后，画面立即恢复到 MCBE 的原生相机。

## 二、架构

### 2.1 数据流

```text
EditorController
  -> CameraSampler::sampleAt(camera, timelineTick)
  -> ReplaySession::setEditorCameraOverride(sample)
  -> EditorCameraOverrideState（mutex 保护的值快照）
  -> LevelRenderer::preRenderUpdate Hook
  -> CameraRenderOverride::apply(state, CameraComponent)
```

### 2.2 模块边界

| 模块 | 职责 | 不负责 |
|---|---|---|
| `ReplaySession` | 发布、清除线程安全的覆盖状态 | 操作 Actor、Player、CameraInstruction 或渲染组件 |
| `CameraRenderOverride` | 在渲染前读取最新状态并写入 CameraComponent 的位置、四元数朝向 | 采样关键帧、时间映射、编辑器选择逻辑 |
| `EditorController` | 根据 Camera 优先级计算 `CameraSample` 并发布状态 | 选择渲染时机 |
| `LevelRenderer` Hook | 调用渲染覆盖模块 | 保存项目状态或修改回放进度 |

### 2.3 覆盖状态

```cpp
struct EditorCameraOverrideState {
    bool active{};
    Vec3 position{};
    float yaw{};
    float pitch{};
    float fov{90.0f};
};
```

- UI/controller 线程仅通过 `ReplaySession::setEditorCameraOverride` 发布完整值快照。
- 渲染线程只复制快照；不保存对编辑器项目模型或 Player 的引用。
- `clearEditorCameraOverride` 只将 `active` 置为 `false`。

### 2.4 渲染覆盖规则

1. Hook 调用原生 `LevelRenderer::preRenderUpdate` 后，读取当前 `EditorCameraOverrideState`。
2. `active=false` 时不写入任何相机状态。
3. 取得当前渲染相机实体的 `MinecraftCamera::CameraComponent`。
4. 将 `position` 写入 `mPosition`。
5. 按 MCBE 坐标约定将 `yaw/pitch` 转换为归一化四元数，写入 `mOrientation`。
6. 首版不写 `mFieldOfView`，但 `applyFov` 私有接口保留为空实现。
7. 每一帧只写一次最终采样值；不写 Player、不创建 CameraInstructionComponent。

### 2.5 错误处理与回退

- ClientInstance、CameraActor、EntityContext 或 CameraComponent 不可用时，本帧跳过，不缓存裸指针，不影响原生渲染。
- 回放不活跃、未进入隔离回放世界、暂停摆机位、样本无效时，控制器清除覆盖状态。
- Hook 安装失败时记录错误并保留既有回放与编辑器功能；不会启用替代的 Player/Instruction 路径。

## 三、执行

### 3.1 实现步骤

1. 在 `ReplaySession` 实现受 mutex 保护的覆盖状态读写，替换当前安全空实现。
2. 新增 `editor/camera-render/CameraRenderOverride.{h,cpp}`，实现状态读取、组件定位与位置/四元数朝向写入。
3. 新增并安装 `LevelRenderer::preRenderUpdate` Hook，保证覆盖发生在原生渲染准备之后、实际 render 之前。
4. 保持 `EditorController` 的采样规则：选中 Camera 优先，否则解析当前 Sequence 段；仅发布有效样本。
5. 为状态读写与欧拉角到四元数转换添加模型测试；构建播放模块并在本地回放验证。

### 3.2 验证步骤

1. 建立两帧位置、yaw 和 pitch 均不同的 Camera。
2. 播放时确认镜头沿采样轨道移动并转向，且无上下抖动。
3. 停止/取消预览，确认原生镜头立即恢复。
4. 连续播放一分钟，确认无显著帧率下降、内存增长或实体位置改变。
5. 校验 `CameraInstructionComponent` 与 `Player::moveTo` 均不出现在新的渲染覆盖调用链。

### 3.3 不变量

1. 渲染覆盖永不修改回放 Player 状态。
2. 每个渲染帧只基于一份完整、互斥保护的状态快照写入。
3. 轨道时间始终使用 `timelineTick`，与 World Actor 的 `sourceTick` 映射保持隔离。
4. FOV 仅作为状态字段保留，首版不改变 MCBE 投影矩阵。
