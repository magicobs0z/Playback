# 回放菜单国际化调试

状态：OPEN

## 症状

回放菜单将国际化键名直接显示在界面中，而不是显示对应的中文文本。

## 假设

1. 当前运行的 DLL 未包含最新语言资源。
2. 语言 JSON 没有被构建产物复制到模组加载路径。
3. 运行时 I18n 未加载 `src/lang` 中新增的 `replayBrowser` 键。
4. `_tr()` 的命名空间或资源域与语言 JSON 的键路径不匹配。

## 证据

- 构建前 `bin/playback/lang/zh_CN.json` 为 4039 字节，修改时间为 2026-07-31，不含 `playback.replayBrowser`。
- 源文件 `src/lang/zh_CN.json` 为 6942 字节，包含 `playback.replayBrowser`。
- 原 `playback` 目标没有语言文件复制步骤；语言复制仅位于未默认构建的 `refactor-model-tests` 目标。
- 修复后 `bin/playback/lang/zh_CN.json` 为 6942 字节，已成功解析出 `playback.replayBrowser.title = 回放列表`。

## 修复

为 `playback` 构建目标增加构建后语言目录复制。

## 验证

- `xmake build playback` 成功。
- 待在游戏中加载新构建产物并打开回放菜单进行运行时确认。
