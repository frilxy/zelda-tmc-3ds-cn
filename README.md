# The Minish Cap 3DS

<img width="1672" height="941" alt="The Minish Cap 3DS" src="https://github.com/user-attachments/assets/db99e777-12a2-4222-86c3-7c8f14062586" />

Simplified Chinese edition of the native Nintendo 3DS dual-screen port of *The Legend of Zelda: The Minish Cap*. The public CIA builds use the Japanese baseline and support the original Japanese ROM and Angel Team SP4.

Made with the help of Codex.

This project is based on open-source work from:

* [samyost1/tmc-android](https://github.com/samyost1/tmc-android) — dual-screen Android source base
* [Project Picori](https://github.com/999sian/tmc) — native Minish Cap engine and port infrastructure
* [zeldaret/tmc](https://github.com/zeldaret/tmc) — original decompilation

No ROM or extracted Nintendo game assets are distributed with this project. You must provide your own legally obtained compatible Game Boy Advance ROM.

## Community

Join my Discord for updates, support, bug reports, testing builds, suggestions, and other Nintendo 3DS homebrew projects:

https://discord.gg/SMW49UMkw

## Features

* Native Nintendo 3DS port with full dual-screen support.
* Chinese touchscreen menus, maps, settings and item controls. Angel Team SP4 supplies the Chinese game text.
* Japanese and SP4 ROM profiles are detected at startup and use separate save files.
* Under 「设置 → 屏幕」, select 「宽屏」 and 「点对点」 for 400×240 Full View. Select 「宽屏+AI」 with 「点对点」 for generated margins in supported scenes. 「原始」 uses the original aspect ratio. Existing Stretch preferences load as Wide.
* The 3D slider controls stereoscopic output. Toggle 「3D景深」 to add depth to gameplay scenery and characters.
* Wide cutscenes continue scripted walks to visible destinations or beyond the edge of the expanded view.
* Bottom-screen interface with live map, dungeon information, quest status, touch item controls, and settings.
  The opening Picori legend keeps the bottom screen black; the overworld map
  shows only its stone frame until Link receives it.
* New 3DS enhancements including 804 MHz mode, L2 cache, multi-core rendering, and optional 2x–5x turbo using the C-Stick.
* Three display styles: Blur, Bilinear, and Pixel Perfect.
* In-app updates with Stable and Pre-release channels, top-screen changelogs, and verified CIA installation.
* Built-in Project Picori Randomizer support with separate normal and randomized save data.
* Native stereo audio, persistent settings, reliable save handling, FPS tools, and diagnostic dumps for bug reports.

## Performance

The port automatically selects a performance profile for the detected console.

**New Nintendo 3DS** keeps the full-presentation path with 804 MHz mode, L2 cache, multi-core rendering, and C-Stick turbo.

**Old Nintendo 3DS** uses an optimized renderer, lower-overhead audio path, and adaptive presentation-skip profile. When rendering cannot sustain 60 visual FPS, the engine can continue advancing near its 60 Hz target instead of slowing the entire game down. Input, touch, audio, and lifecycle handling continue to run on skipped presentation ticks.

The bottom-screen worker avoids redundant static redraws while keeping touch input sampled on every engine tick.

## Installation

1. Install the JP CIA with FBI. The releases page lists the stable and prerelease builds and links to their installation QR codes. [Releases](https://github.com/frilxy/zelda-tmc-3ds-cn/releases)

2. Create this folder on your SD card:

```text
sdmc:/3ds/The Minish Cap 3DS/
```

3. Place a supported `.gba` ROM in that folder. Use Angel Team SP4 for Chinese game text.

| ROM | SHA-1 |
|---|---|
| Original Japanese | `6c5404a1effb17f481f352181d0f1c61a2765c5d` |
| Angel Team SP4 | `ba04cfbe93d12d2ad684c52234472fa12a5b53d7` |

The launcher accepts any `.gba` filename and matches the ROM by hash. See the platform guide for USA and European ROM builds. [3DS platform guide](platform/3ds/README.md)

The ROM stays on your SD card and is never included in the CIA.

### Audio

Audio requires a working Nintendo 3DS DSP firmware setup.

If homebrew audio is not working, open the Luma3DS Rosalina Menu and use:

```text
Miscellaneous options > Dump DSP firmware
```

## Diagnostics

If you encounter a crash, graphical bug, performance problem, or anything unusual, press:

```text
L + R + A
```

The port will pause and create a diagnostic dump containing screenshots, memory information, runtime state, performance data, and other information that can help identify the problem.

Dumps are saved under:

```text
sdmc:/3ds/The Minish Cap 3DS/dumps/
```

Gameplay dumps also include a validated `load-state.bin` checkpoint. To reproduce the most recent dump, open:

```text
Settings > Developer > Load State
```

New sessions use `000-dump-YYYYMMDD-HHMMSS`, `001-dump-YYYYMMDD-HHMMSS`, and so on.
The sequence survives restarts and clock changes; an emptied dumps folder starts again at 000.

The loader asks for confirmation, selects the newest numbered folder (or a legacy `dump-*` folder), and rejects corrupted checkpoints or checkpoints
created with a different ROM region. Older dumps that only contain `save-state.bin` remain supported, but they resume
from the saved checkpoint rather than the exact captured position.

Please send the dump when reporting bugs whenever possible.

## Updates

Open **Settings > Update**. Select **Stable** or **Pre-release**, then tap the
release version to read its changelog on the top screen. Use **Prev / Next** to
turn pages. **Download Update** asks for confirmation before downloading and
installing. Save your game first: the application closes after a successful update.

CIA builds overwrite only this application's title after checking its ID and
SHA-256 digest.
Wi-Fi is required; connection failures leave the current installation available.

## Releases

Every GitHub release includes:

* Installable JP CIA
* FBI QR code
* SHA-256 checksum and source code archive

Latest release:

https://github.com/frilxy/zelda-tmc-3ds-cn/releases/latest

## Building

Requirements:

* devkitPro
* devkitARM
* libctru
* Citro2D
* Citro3D
* CMake
* `makerom` and `bannertool` for CIA packaging

Build with:

```sh
chmod +x platform/3ds/build.sh
TMC3DS_REGION=JP ./platform/3ds/build.sh
```

The installable CIA is generated at

```text
build-3ds/game/tmc-3ds-v<version>.cia
```

The build does not include or embed a ROM.

## Credits

* [samyost1/tmc-android](https://github.com/samyost1/tmc-android) — dual-screen Android source base used for this port
* [Project Picori](https://github.com/999sian/tmc) — native Minish Cap engine and port infrastructure
* [Raekwon1603/tmc-android](https://github.com/Raekwon1603/tmc-android) — Android packaging and platform work behind the dual-screen fork
* [zeldaret/tmc](https://github.com/zeldaret/tmc) — original decompilation
* EstebanPdN developed the upstream Nintendo 3DS port. [Upstream repository](https://github.com/EstebanPdN/zelda-tmc-3ds)
* tinaut1986 contributed Full View, stereoscopic 3D, GPU improvements, AI scene extensions and gameplay fixes. [Fork v0.4.7](https://github.com/tinaut1986/zelda-tmc-3ds/tree/v0.4.7)

## License and Legal Notice

Source code is distributed under GPL-3.0. See [LICENSE](LICENSE).

Third-party components retain their respective licenses as listed in [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).

Nintendo owns *The Legend of Zelda*, *The Minish Cap*, and all associated game content.

This is an unofficial fan-made project and is not affiliated with or endorsed by Nintendo.

No ROM, extracted Nintendo game assets, save data, or firmware is distributed with this project.
