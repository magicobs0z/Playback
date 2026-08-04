# 渲染相机覆盖调试

状态：OPEN

## 症状

- 回放预览中摄像机轨道不会改变最终画面。
- 早期同步 HTTP 观测造成渲染线程卡顿；移除后游戏恢复流畅。

## 假设

1. `LevelRenderer::preRenderUpdate` Hook 没有触发。
2. 编辑器控制器没有发布有效的相机覆盖状态。
3. 通过 `getCameraEntity()` 定位的 ECS 实体不是最终渲染相机。
4. `ClientInstance::getCamera()` 可在渲染前 Hook 中读取并作为最终渲染覆盖入口。

## 已有证据

- HTTP 观测确认 Hook 触发，且 Controller 已发布有效样本。
- `getCameraEntity()` 对应实体没有 `CameraComponent`，假设 3 已确认。
- 同步 HTTP 位于渲染线程会造成可见卡顿，后续禁止在渲染路径执行网络请求。

## 下一步

- 以无阻塞本地计数方式验证 `getCamera()` 可读。
- 验证后直接在该对象上覆写位置与正交朝向基，并调用视图依赖更新。
