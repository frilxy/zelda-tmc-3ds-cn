# The Minish Cap 3DS Platform

This target builds the native dual-screen Nintendo 3DS frontend.

Outside gameplay, the bottom screen shows a procedural gold-framed Triforce;
touching it opens the existing Minish Cap Settings hierarchy.

## Console Installation

Install the CIA built for your ROM family. GitHub Releases provides the JP build for clean Japanese and Angel Team SP4 ROMs. [Releases](https://github.com/frilxy/zelda-tmc-3ds-cn/releases)

Then create this directory on the SD card:

```text
sdmc:/3ds/The Minish Cap 3DS/
```

Place a legally obtained supported ROM there. Any `.gba` filename is accepted.
The port verifies the complete ROM hash and activates the matching data/text
profile before boot. The default USA-baseline package accepts the original USA
and Europe ROMs. A `TMC3DS_REGION=JP` package accepts clean Japan and Angel SP4;
the launcher rejects cross-family ROM/build combinations instead of starting a
known hybrid configuration. Expected SHA-1 values:

```text
USA:       b4bd50e4131b027c334547b4524e2dbbd4227130
Europe:    cff199b36ff173fb6faf152653d1bccf87c26fb7
Japan:     6c5404a1effb17f481f352181d0f1c61a2765c5d
Angel SP4: ba04cfbe93d12d2ad684c52234472fa12a5b53d7
```

Only that exact Angel Team Chinese SP4 revision is enabled. Unknown Japanese
patches fail closed and the startup console prints their SHA-256.

The ROM is read locally from the SD card and is never copied into the CIA.

Audio requires a working 3DS DSP firmware setup. On Luma3DS, use Rosalina's
`Dump DSP firmware` option if homebrew audio is unavailable.

## Display

- Top-screen aspect ratios are Wide (「宽屏」), Wide + AI (「宽屏+AI」), and Original (「原始」). Existing Stretch preferences load as Wide.
- Display styles are Blur, Bilinear, and Pixel Perfect (「点对点」). Fresh installations start with Wide and Bilinear. Both Wide aspect ratios enter 400×240 Full View with Pixel Perfect. Wide + AI also adds generated margins to supported scenes.
- The 3DS slider controls stereoscopic depth. The 「3D景深」 setting enables additional depth for scenery and characters.
- Bottom screen: 320x240 map, dungeon/status information and touch item UI.
- Rendering: PICA200 GPU renderer with a software GBA PPU fallback.
- Performance profile: selected automatically from the detected console model.
- New 3DS: requests 804 MHz, L2 cache and access to the extra application core.
- New 3DS turbo: hold the C-stick in any direction and select 2x through 5x
  game speed from the Gameplay settings.
- Old 3DS: experimental CPU-renderer and audio optimizations plus adaptive
  presentation skipping target 60 Hz engine timing when visual rendering falls
  behind. Input, touch, audio and lifecycle processing continue on skipped
  presentation ticks; visual FPS may be lower.
- Bottom-screen scheduling: touch state is sampled on every engine tick;
  interactive hitboxes are promoted with the physically visible buffer, and
  unchanged static pages avoid redundant paint/upload work.
- Settings: Minish Cap-themed Screen, Gameplay, Developer, and Randomizer submenus with
  persistent options, a manual memory-dump command, and a live diagnostics
  overlay.
- Randomizer: persistent Project Picori randomizer mode in its own submenu. Mode
  changes require confirmation, clear only the active profile and related
  state, keep the ROM, and restart with isolated normal/randomized saves.
- Show FPS: measured presentation cadence in a compact lower-left top-screen
  box.
- Diagnostics: press `L + R + A` to pause the game, display `DUMP SAVED`, and
  create `dumps/dump-*` with top and bottom physical-framebuffer BMP and raw
  captures, EWRAM, IWRAM, VRAM, palettes, OAM, I/O and game state, frame
  visual and engine cadence, adaptive-skip data, per-core PPU timings, GPU
  work, audio buffer health, save
  persistence state, memory availability, lifecycle state and complete input
  data.
- System lifecycle: HOME, sleep and application close events are handled by the
  regular 3DS applet loop.
- Startup: the top screen shows the game logo and the bottom screen shows a rotating gold Triforce during loading.

The CIA metadata uses a stable title ID and requests SD card access for local
ROM and save data.

## Requirements

- devkitPro with devkitARM, libctru, Citro2D and Citro3D
- CMake with the devkitPro Nintendo 3DS toolchain
- `makerom` and `bannertool` for CIA packaging

Run:

```sh
chmod +x platform/3ds/build.sh
platform/3ds/build.sh
```

For the JP/SP4 compile-time baseline and a symbol-friendly debug build:

```sh
xmake f -y --game_version=JP
xmake build -y asset_processor
tools/bin/asset_processor extract JP build/JP/assets
TMC3DS_REGION=JP TMC3DS_BUILD_TYPE=Debug platform/3ds/build.sh
```

Run the asset processor with clean `baserom_jp.gba` in the repository root.
It generates `build/JP/assets/map_offsets.h` and `gfx_offsets.h`. The repository includes these JP offset headers for CIA builds. The linker writes `tmc-3ds.map` beside the ELF.

The region-baseline packages are written under `build-3ds/game/`. No ROM,
extracted asset package or save data is included in either package.
