# 更新日志（中文版）

本文件是 GitHub Release 说明的唯一来源，也供应用内「设置 > 更新」的更新日志页面
读取（上屏 42 列、自动换行，只渲染 ASCII 字符）。

**格式约定**：中文说明放在文件前部；文件末尾的「Changelog」小节必须是纯 ASCII
条目——上屏字体只覆盖 ASCII，非 ASCII 字节会被更新器直接丢弃。请勿在正文里写出
带井号的 Changelog 标题，否则更新器会从那里开始截取。每条 ASCII 条目控制在 2 行
（≤84 字符）以内。

---

## v2.1.1

### 修复

- 应用内更新的目标仓库改为 `frilxy/zelda-tmc-3ds-cn`。此前仍指向上游
  `EstebanPdN/zelda-tmc-3ds`，中文版点「检查更新」会下载上游英文包并覆盖本应用，
  装完即失去 Angel SP4 中文 ROM 支持。
- 发布标签改用标准版本号 `v2.1.1`。旧的 `v2.1c` 不符合更新器的版本语法，检查更新
  永远失败。
- 发布转为稳定版通道。`v2.1c` 是预发布，`/releases/latest` 返回 404，稳定通道此前
  实际不可用。
- 发布资产改为更新器要求的平铺命名：`tmc-3ds-v2.1.1.cia` 与 `.3dsx`，不再打包成
  zip，附带 `SHA256SUMS.txt` 与 FBI 二维码。

## v2.1c

基线版本：上游 v2.1 + 下屏中文 UI + Angel Team SP4 中文 ROM 支持。

## Changelog

- In-app updates now target frilxy/zelda-tmc-3ds-cn
  instead of the upstream English repository.
- Version tags use standard semver; v2.1c could
  never be parsed by the updater.
- Releases are stable by default, so
  releases/latest resolves.
- Assets are flat and updater-ready: CIA, 3DSX,
  SHA256SUMS.txt and an FBI QR code.
