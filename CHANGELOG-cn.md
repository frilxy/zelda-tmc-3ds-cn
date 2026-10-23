# 更新日志（中文版）

## v2.2.1

本版把下屏 UI 的调整合并进主线，版本号仍为 2.2.1。

### 字体

- 正文 Fusion Pixel 12px，面板标题与标签页 GNU Unifont 16px
- 字形改为 16×16 2bpp 打包，轮廓烘焙进位图
- 新增 `scripts/dev/gen_cn_font.py`：扫描下屏源码自动生成字符集

### 配色与布局

- 楼层方块：黑框 + 紫填充，方块相互紧贴，文字 1px 偏移加粗
- 当前预览的楼层带红白选择框，与物品栏选中标记同一套
- 区域名、物品标题、设置标题、缩放/返回：深紫外框 + 淡青内框 +
  浅蓝填充 + 纯白文字，四角圆角
- 设置行数值去掉底框，改为右对齐深色文字
- 标签栏按钮向下增高，设置齿轮按钮去掉金色焦点框
- 底部「返回」按钮与更新页文案汉化

### 修复

- 白字显示成黄色：主题文件误用了 GBA 15 位 `RGB()` 宏
- **下屏所有数字不显示**：JP 构建下 `sFontOk` 恒为 0，ASCII 数字一个都不画
  ——海螺数量、幸福碎片数量因此空白。现在改为退回中文字库（已含 ASCII）
- 选中楼层的选择框改为红色（游戏原图是绿色）
- 海螺、幸福碎片等计数改用 HUD 的 8x16 数字精灵，与弹药角标同款
- 室内地图的钥匙并排放在卢比左侧，两者都用缩小后的尺寸；chip 只占
  一行，上方装备圈因此保持最大
- 室外地图没有钥匙，卢比利用空出的位置居中
- 室内地图钥匙与卢比改为各自居中、上下排列，用掉底部余量
- 修正中文/ASCII 字形绘制偏移：字形按 16px 单元格贴图，步进却用更窄的
  advance，导致每个字右移 (16-adv)/2，居中标签看起来左侧留白
- 海螺计数也改用 HUD 数字精灵（此前只有幸福碎片改了）

## Changelog

- Bottom-screen font, palette and layout
  work.
- Selected floor wears the same red
  cursor brackets as the items grid.
- White text rendered yellow: the theme
  file had the GBA 15-bit RGB() macro.

