# OreUI 选择遥测

> 入口：[`d:\raplay\Playback\src\playback\functions\telemetry\OreUiSelectionHooks.cpp`](file:///d:/raplay/Playback/src/playback/functions/telemetry/OreUiSelectionHooks.cpp)

## 需求

- 在不改变客户端 UI 行为、参数、返回值或资源加载结果的前提下，记录原生页面选择的 UI 技术栈。
- 记录 `ScreenTechStackSelector::getTechStackForScreen` 的屏幕名与返回的 `JsonUI` / `OreUI`。
- 记录 `OreUI::SceneProvider::createScene` 的 URL、`RouteMode`、`FacetRegistryLocation` 与场景创建是否成功。
- 记录 Router 成功变更后的旧/新路径、查询参数和片段，以及 Push、Replace、Back 请求及其结果。
- 重复调用仅在首次观察到或结果变化时输出调试日志，避免常规 UI 刷新造成日志洪泛。
- 将安装状态和观察结果追加写入 `<模组数据目录>/telemetry/oreui-selection.txt`，便于宿主日志不可见时直接验证 Hook。
- 任一 Hook 无法安装时独立降级；遥测失效不得阻止回放模组启动、启用或卸载。

## 架构

```
ScreenTechStackSelector::getTechStackForScreen
    -> 原函数
    -> OreUiSelectionTelemetry: screenName -> TechStack

OreUI::SceneProvider::createScene
    -> 原函数
    -> OreUiSelectionTelemetry: url + mode + location -> created

OreUI::Router::_onChange
    -> 原函数
    -> OreUiSelectionTelemetry: old/current path + query + fragment

OreUI::Router::_pushRoute / replaceRoute / goBack
    -> 原函数
    -> OreUiSelectionTelemetry: navigation request + result
```

- `OreUiSelectionHooks` 是 `playback::functions` 下独立的 Hook 模块，仅对外暴露 `hookOreUiSelectionTelemetry(bool enable)`。
- 六个 `LL_TYPE_INSTANCE_HOOK` 分别绑定选择器、场景提供者和四个 Router 成员函数签名。
- Detour 始终先调用 `origin(...)`，只读取传入参数与原函数结果；不修改 Router、SceneStack、配置对象或页面资源。
- `RouterLocation` 仅通过公开的路径、查询和片段访问器读取，避免依赖 SDK 布局占位成员。
- 模块用本地状态记录六个 Hook 的安装状态，并在安装失败时逆序回滚；卸载失败时恢复已移除 Hook，保持生命周期对称。
- TXT 写入通过进程内互斥、创建 `telemetry/` 目录和追加写入完成；写入失败只记录一次警告，绝不影响客户端 UI 调用。
- `Playback::hook()` 在必需网络和 Tick Hook 成功后尝试安装遥测 Hook；`Playback::unhook()` 在 Tick Hook 移除前先卸载遥测 Hook。

## 执行

1. 新增 `OreUiSelectionHooks.h/.cpp`，实现六个只读成员 Hook、重复日志抑制和独立安装状态。
2. 在 `Playback.cpp` 接入安装、失败降级和卸载路径。
3. 用 xmake 构建 `playback` 客户端目标，确认 SDK 成员函数签名和 Hook 宏可编译。
4. 启动 `1.26.10.04` 客户端，先检查 `<模组数据目录>/telemetry/oreui-selection.txt` 是否出现 Hook 安装状态，再进入 OreUI 页面、执行页面跳转和返回，检查技术栈选择、场景 URL 与 Router 事件。
5. 重复打开页面、返回主菜单并禁用模组，确认 UI 行为不变且卸载日志无 Hook 残留。
