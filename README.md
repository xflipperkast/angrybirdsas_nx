
<div align="center">

<img src="icon.jpg" alt="angrybirdas_nx" width="160">

# angrybirdas_nx

**Angry Bird Epic All Stars on Nintendo Switch**

An unofficial Nintendo Switch port of the Android version of  
**Angry Bird Epic All Stars — 1.2.8.1**

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Ko-fi](https://img.shields.io/badge/Support_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>
---

## About

`angrybirdas_nx` is an experimental port of Angry Bird Epic All Stars Android 1.2.8.1 to Nintendo Switch.

The project provides the Switch-side compatibility layer needed to run the Android Unity/IL2CPP game code under Horizon OS.

> This repository does not include Angry Bird Epic All Stars game assets or other proprietary files.

---

## Build

### Requirements

- devkitPro
- devkitA64
- libnx
- GNU Make
- The original `angrybirdas_nx` Makefile / linker setup
- Your own legally obtained Angry Bird Epic All Stars Android 1.2.8.1 APK

### Compile

Clone the repository:

```sh
git clone <repository-url>
cd angrybirdas_nx
```

Make sure your devkitPro environment is configured:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
```

Then build:

```sh
make -j
```

To rebuild from scratch:

```sh
make clean
make -j
```

> Some source-only development packages may not include the original Makefile, linker configuration, or `elf2nro` setup.
> If they are missing, use the build files from your existing working `angrybirdas_nx` environment. Hold **R** while opening Homebrew Menu through a game, or run the port directly as a forwarded app. Applet mode does not provide enough memory for this port.

---

## Running

Place the generated `.nro` and your game APK in the `angrybirdas_nx` folder on the SD card:

```text
sd:/switch/angrybirdas_nx/
├── angrybirdas_nx.nro
└── game.apk
```

Launch the app and the installer will prepare the runtime files automatically.

Controller cursor controls:

- `+` — show the on-screen cursor
- `-` — hide the on-screen cursor
- Left stick — move the cursor
- `A` — tap; hold `A` while moving the stick to drag
- Touchscreen — remains available in handheld mode
- 
---

## Support

If you like the project and want to support development:

---

## Disclaimer

This is an unofficial fan-made project and is not affiliated with or endorsed by Rovio Entertainment, Chimera Entertainment, or the original rights holders.

Angry Birds, Angry Birds Epic, related trademarks, artwork, audio, game assets, and other copyrighted material belong to their respective owners.

## Contribution

If you can make useful updates to the port, such as fixing a bug, improving performance, or adding compatibility features, feel free to help the port run better. Open an issue for problems or submit a well-documented pull request for code changes.
