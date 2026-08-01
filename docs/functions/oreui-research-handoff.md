# OreUI 客户端研究交接

> 状态：暂停。本文记录 `1.26.10.04` Windows 基岩版客户端中已验证的 OreUI 运行链、当前代码和后续建议，供恢复研究时使用。

## 需求

- 最终目标是把 Playback 的回放菜单从当前 JsonUI 迁移到客户端内置 OreUI。
- 研究对象是 Windows 基岩版客户端 Native Mod；不涉及 BDS 服务端。
- 采用客户端内置 OreUI 渲染，后续需要验证自定义资源、路由注册、页面创建和 C++ 与前端的数据事件桥。
- 当前阶段仅完成只读运行时观测；未改写客户端路由，未注入自定义 OreUI 页面，未迁移回放菜单。

## 架构

### 当前 Playback UI

- 回放菜单仍为 JsonUI：资源位于 `Playback/resources/ui/`，由 `MainMenuHooks.cpp` 注册 JSON UI binding 并通过 `displayJsonDefinedControlPopup(...)` 打开 `playback_replay_browser`。
- 该弹窗不能直接切换为 OreUI：它没有 OreUI URL、前端 bundle、路由注册或数据桥。

### 已验证的客户端 OreUI 链路

```text
原生菜单操作
    -> OreUI::Router::_pushRoute / replaceRoute / goBack
    -> OreUI::Router::_onChange
    -> OreUI::SceneProvider::createScene（需要创建 Web 场景时）
    -> /hbui/index.html 等客户端内置资源
```

- 客户端实际 OreUI Web 资源根目录：`D:\raplay\1.26.10.04\data\gui\dist\hbui`。
- 路由清单：`D:\raplay\1.26.10.04\data\gui\dist\hbui\routes.json`。
- `data/resource_packs/oreui` 主要是资源包标识、语言和归档，不是可直接新增 HTML/JS 页面并自动路由的页面根。
- 已知场景创建 URL：`/hbui/index.html`，在 `OutOfGame` 环境可创建成功。

### 已实现的只读遥测

- 模块入口：[OreUiSelectionHooks.cpp](file:///D:/raplay/Playback/src/playback/functions/telemetry/OreUiSelectionHooks.cpp)。
- 生命周期由 [Playback.cpp](file:///D:/raplay/Playback/src/playback/Playback.cpp) 管理；遥测安装失败不会阻断 Playback。
- 观测结果写入 `<Playback 模组数据目录>/telemetry/oreui-selection.txt`。
- 当前 Hook：
  - `ui::ScreenTechStackSelector::getTechStackForScreen`
  - `OreUI::SceneProvider::createScene`
  - `OreUI::Router::_onChange`
  - `OreUI::Router::_pushRoute`
  - `OreUI::Router::replaceRoute`
  - `OreUI::Router::goBack`
- Hook 均先执行原函数，仅记录参数与结果；不改变 UI 行为、路由参数、返回值或资源加载。

## 执行记录

### 已确认的运行时证据

客户端已产生以下关键观测：

```text
scene  url=/hbui/index.html  route_mode=0  location=1  created=true
route_push  route=/play/all?dirtyLevelId=  push_mode=0  success=true
route_change  old_path=/__bedrock__/start_screen  current_path=/play/all
route_replace  route=/oreui-settings/account  success=true
route_back
route_push  route=/realms-plan-picker  push_mode=1  success=true
```

结论：

- `/hbui/index.html` 已由客户端成功创建；`route_mode=0` 为 `RouteMode::None`，`location=1` 为 `FacetRegistryLocation::OutOfGame`。
- Router 遥测已覆盖普通 Push、Replace、Back 和 `Flux` Push。
- 请求路径可能被路由器重定向，例如 `/settings/default` 最终进入 `/oreui-settings/accessibility`；后续验证必须同时看请求事件和 `_onChange` 的最终路径。

### 构建与部署

- 使用 `xmake build -j 1 playback` 构建成功。
- 并行构建曾在既有 `EditorBridge.cpp` 触发 MSVC `C1060` 堆空间不足；单线程构建可规避该环境问题。
- 构建产物为 `Playback/bin/playback/playback.dll`，部署目标为 `D:\raplay\1.26.10.04\mods\playback\playback.dll`。

## 后续执行

### 第一阶段：主动导航 PoC

目标：由 Playback 主菜单按钮发起一次原生 OreUI 导航，验证插件发起导航的时机和 Router 生命周期。

- 已选定目标路由：`/play/all?dirtyLevelId=`。
- 采用一次性 `replaceRoute`，不用私有 `_pushRoute` 主动导航，以降低 ABI 风险。
- 仅允许首次点击回放按钮产生请求；第二次及后续点击不重复执行。
- 执行应脱离按钮回调栈，在主菜单 tick 中执行，避免 UI 回调重入。
- 记录 PoC 请求、Router 不可用、`replaceRoute` 返回值和最终 `route_change`。
- PoC 完成后应恢复“回放”按钮当前打开 JsonUI 浏览器的语义，或另设专用调试入口。

重要限制：SDK 没有公开、类型安全的“获取当前 OutOfGame Router”全局 accessor。已验证的 `Router&` 只来自 `SceneProvider::createScene` 回调，不能跨生命周期缓存裸引用。若继续实现 PoC，需要先设计 Router 实例生命周期追踪，或仅在受控回调时机内调用。

### 第二阶段：自定义资源 PoC

只有第一阶段明确主动导航与实例生命周期后才开始。

1. 分析 `hbui/routes.json` 的路由注册与资源发现规则。
2. 验证客户端是否支持由模组资源包覆盖或合并 `hbui` 页面、路由和 bundle。
3. 创建最小空页面，确认自定义 URL 能被 matcher 接受并创建场景。
4. 不直接修改原版哈希 bundle；客户端更新会覆盖文件，且 HTML、JS、CSS 哈希引用必须保持一致。

### 第三阶段：回放菜单迁移

1. 定义 C++ 到 OreUI 的只读回放列表数据桥。
2. 定义 OreUI 到 C++ 的选择、删除、播放、返回事件桥。
3. 先迁移列表展示和返回，再迁移编辑、删除和播放操作。
4. 保留 JsonUI 作为实验期间的可用回退，直到 OreUI 页面、路由和数据桥稳定。

## 风险与约束

- SDK 中的 `Router::_onChange` 和 `_pushRoute` 属于内部成员，版本升级可能导致 ABI、符号或调用路径变化；目标版本当前锁定为 `1.26.10.04`。
- 不要通过 `TypedStorage` 偏移读取 `WorldSystem` 或 Router 内部成员；这无法可靠区分 InGame/OutOfGame 实例且版本耦合极强。
- 不要缓存从 `createScene` 获得的 `Router&`；菜单销毁、Router 重建或世界切换后会悬空。
- 不要在 Router Hook 内再次导航；会产生重入、重复通知或破坏路由历史。
- Router Hook 当前同步追加并 flush TXT；若后续扩大观测范围，应考虑缓冲写入以避免主线程 I/O 放大。
- 恢复研究前，应先以原生页面操作重新确认 `oreui-selection.txt` 中存在 `status\thooks=installed` 和 Router 事件，再开始行为变更实验。
