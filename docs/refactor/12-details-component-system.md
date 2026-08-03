# 12 · Details 组件化属性面板

## 一、需求

将 Details 面板重构为可程序化组合的属性检查器。视觉参考 Unreal Details：标题带、对象层级带、搜索带、可折叠属性分组、标签/值属性行和低对比度细线分隔。

组件系统必须覆盖 Sequence、Sequence Segment、World Actor、World Actor Segment、Sub Actor、Camera、Keyframe、Marker 与空状态；所有可编辑字段仍经 `EditorAction → EditorController → CommandStack` 提交。

所有组件使用编辑器局部字体比例、统一内边距、统一行高和统一语义色。Details 内容必须可滚动，窄窗口下标签和值列按可用宽度自适应，不能挤出面板。

## 二、架构

`DetailsPanel` 仅负责读取当帧项目快照、解析 Selection、选择领域视图和提交 Action。

`PropertyControls` 提供无领域依赖的 UI 原语：标题栏、对象树行、搜索栏、工具图标、分组头、属性行、文本/数值/下拉/开关控件、状态消息和满宽操作按钮。

领域视图按业务边界组合原语：

- `SequenceDetailsView`：Sequence、Sequence Segment。
- `WorldActorDetailsView`：World Actor、World Actor Segment、Sub Actor。
- `CameraDetailsView`：Camera、Keyframe、Marker。

领域视图只接收项目快照、选择模型和提交回调；通用控件不依赖 `EditorStateExt`、Selection 或 `EditorAction`。

## 三、执行

1. 新建 `PropertyControls`，实现统一的检查器布局和自适应度量。
2. 新建三个领域视图，迁移现有 8 类上下文与已有 Action 提交语义。
3. 收缩 `DetailsPanel` 为项目守卫、滚动容器和选择分派。
4. 使用已有 Lucide 图标提供 Add、Search、Settings 等工具按钮；没有后端能力的操作保持禁用。
5. 运行 `xmake build playback`、`xmake build refactor-model-tests`、`xmake run refactor-model-tests` 和 `git diff --check`。
