# 3DS 平台说明

本文以 GitHub 正式发行版的源码和安装包为准。功能和移植来源见项目首页。 [项目首页](../../README.md) · [下载正式版](https://github.com/frilxy/zelda-tmc-3ds-cn/releases/latest)

## 安装和区域基线

**中文剧情需要准确匹配的 Angel Team SP4 汉化 ROM。** 请先用 FBI 安装 JP CIA，再把自行合法取得的 SP4 `.gba` 文件放进 SD 卡的 `sdmc:/3ds/The Minish Cap 3DS/`。文件名可自定，扩展名须为 `.gba`。CIA 读取 SD 卡上的 ROM，发行包仅提供移植程序。

启动时程序会通过 ROM 哈希选择对应的数据和文本配置。公开发行的 CIA 使用 JP 基线。自行构建时可按 ROM 的区域选择基线。

| ROM | 可用构建基线 | SHA-1 |
|---|---|---|
| Angel Team SP4 汉化版 | JP | `ba04cfbe93d12d2ad684c52234472fa12a5b53d7` |
| 日版原版 | JP | `6c5404a1effb17f481f352181d0f1c61a2765c5d` |
| 美版原版 | USA 或 EU | `b4bd50e4131b027c334547b4524e2dbbd4227130` |
| 欧版原版 | USA 或 EU | `cff199b36ff173fb6faf152653d1bccf87c26fb7` |

日版原版和 SP4 汉化版各有独立存档。启动提示会拒绝未知修改版或与当前构建基线不匹配的 ROM。

## 屏幕和性能

- 上屏宽高比可选「宽屏」「宽屏+AI」「原始」。显示方式可选「模糊」「双线性」「点对点」。首次安装默认采用「宽屏」和「双线性」。旧版「拉伸」会自动读取为「宽屏」。
- 「宽屏」或「宽屏+AI」配合「点对点」，在对应场景使用 400×240 Full View。在「宽屏+AI」下，部分场景还会显示 AI 补景。
- 支持立体显示的主机可用 3D 滑杆调节立体效果。「3D景深」额外控制游戏场景和角色的景深。
- 下屏为 320×240，提供地图、地牢信息、状态栏与触屏道具操作。加载时下屏显示旋转的金色三角力量。
- 渲染使用 PICA200 GPU，部分画面会回退到软件 GBA PPU。New 3DS 启用 804 MHz、L2 缓存与额外应用核心，并支持 C-Stick 触发 2～5 倍速。旧机使用自适应跳帧，画面帧率可低于引擎的 60 Hz 目标，跳过画面时仍处理输入、音频和触屏。

如无声音，可在 Luma3DS 的 Rosalina 菜单中选择 `Miscellaneous options → Dump DSP firmware`。

## 实时下屏（预发布分支）

- 进游戏后默认显示地图。按 START 或点左上页签切换「物品与收集」，上屏继续运行。ZL／ZR 在下屏不执行翻页。
- 点亮道具格后，底部黑栏显示该 ROM 的原版名称。按实体 A／B／X／Y 绑定到相应位置，绑定的那次按键只设置道具。X／Y 绑定跟随游戏存档。
- 地图总览点区域可打开详情，拖动可查看大图，按 B 回总览。地牢楼层可触摸切换，底部显示建筑名。
- 幸福碎片和剑技可以从收集区打开。点已学卷轴显示原版说明，点石板右上角返回。

[查看下屏实拍](../../docs/screenshots/3ds-bottom-screen.png)

## 诊断

游戏仍有响应时按 `L + R + A`，程序会写入截图、运行状态、内存和性能数据。目录为 `sdmc:/3ds/The Minish Cap 3DS/dumps/`。如果本次诊断包含可恢复的检查点，可在「设置 → 开发者选项」中选择加载状态。

报告问题时请注明 CIA 版本、主机型号、ROM 类型及复现步骤。提交前可按需检查诊断文件，选择与问题有关的资料。 [提交问题](https://github.com/frilxy/zelda-tmc-3ds-cn/issues)

## 构建

从所需的正式版本标签检出源码，与 GitHub 上的安装包对应。需要 devkitPro、devkitARM、libctru、Citro2D、Citro3D、3DS zlib、CMake，以及用于打包的 `makerom` 和 `bannertool`。在仓库根目录执行以下命令生成 JP CIA。

```sh
TMC3DS_REGION=JP ./platform/3ds/build.sh
```

安装包输出到 `build-3ds/game/tmc-3ds-v<版本>.cia`。源码已包含 JP 构建所需的资源偏移表。构建阶段无需提供 ROM，运行阶段请自行将受支持的 ROM 放入 SD 卡。
