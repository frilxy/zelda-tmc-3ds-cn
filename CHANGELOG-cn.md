# 更新日志（中文版）

## v2.2.1

本版把下屏 UI 的调整合并进主线，版本号仍为 2.2.1。

### 下屏字体

- 正文 Fusion Pixel 12px，面板标题与标签页 GNU Unifont 16px
- 字形改为 16×16 2bpp 打包，轮廓烘焙进位图
- 新增 `scripts/dev/gen_cn_font.py`：扫描下屏源码自动生成字符集，
  改文案后重跑即可，不再手工维护字表

### 下屏配色与布局

- 楼层方块：游戏地图配色 —— 黑框 + 紫色填充，方块相互紧贴，
  文字 1px 偏移加粗
- 区域名、物品标题、设置标题、缩放/返回：深紫外框 + 淡青内框 +
  浅蓝填充 + 纯白文字
- 设置行数值去掉底框，改为右对齐深色文字
- 标签栏按钮向下增高，设置齿轮按钮去掉金色焦点框
- 底部「返回」按钮与更新页文案汉化

### 修复

- 上屏 R 键提示画出回旋镖 / 镜盾：动作标签的语言基准表运行时为零，
  帧号退化成动作值，落到物品图标上。现在读到零会用内置表兜底
- 荷叶（水滴神殿）不再静止不动
- 白字显示成黄色：下屏主题文件误用了 GBA 15 位 `RGB()` 宏

## Changelog

- Bottom-screen font, palette and layout
  work merged into the mainline; the
  version stays 2.2.1.
- HUD action labels fell back to item
  icons; the zero language base table is
  now replaced by the built-in one.

