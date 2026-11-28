# KaiDump

KaiDump is a title dumper for Switch 2 consoles running kaiHEN. It has a simple
graphical interface for choosing a game, selecting its content, and exporting
it to an SD card or computer.

The project is still experimental, but Switch 1 and Switch 2 titles, updates,
DLC, ExeFS, and RomFS are supported. KaiDump only reads game content and does
not modify games or NAND.

## Features

- Grid and list views with game names and icons
- Base game, update, and DLC selection
- Extracted files, TAR archives, and NUP packages
- Export to SD card, FTP, or USB

TAR is the quickest single-file format. NUP takes longer to prepare because it
needs to inspect and hash every file first.

## Requirements

- kaiHEN 20261117b or newer
- Enough free SD card space when exporting locally
- A LAN connection for FTP, or a USB data cable for USB transfers

## Installation

1. Download `KaiDump.nro` from the latest release.
2. Copy it to `sdmc:/switch/KaiDump/KaiDump.nro`.
3. Start kaiHEN, open hbmenu, and launch KaiDump.

## Usage

1. Select a game from the title browser.
2. Choose the game content and DLC you want to export.
3. Select Extracted, TAR, or NUP.
4. Choose SD card, FTP, or USB as the destination.
5. Review the selection and start the export.

If a title fails, try starting kaiHEN through that title, then launch KaiDump from the same hbmenu session and retry.

SD card exports are saved under:

```text
sdmc:/switch/KaiDump/dumps/Game title [Title ID]/
```

Extracted and TAR dumps put the game in `game/` (`exefs/`, `romfs/`,
`control.nacp` and the game's icons) and DLC in `dlc/`. Nintendo Switch 2
Editions contain two versions of the game, which go in `game_switch1/` and
`game_switch2/`; keep both, as some Switch 2 versions use data from the
Switch 1 version. NUP files hold the Switch 1 version of these games.

FTP shows the address to connect to once the server starts. FTP access is
read-only, so files cannot be uploaded or deleted. Set your FTP client to
binary transfers (in FileZilla: Transfer > Transfer type > Binary). In ASCII
or "Auto" mode, clients change line endings on their side, which also
corrupts files without an extension such as ExeFS binaries, so KaiDump
refuses downloads in ASCII mode.

For USB, connect the console after the USB screen appears and open the
`KaiDump` device on your computer. Windows supports MTP directly. Linux can use
libmtp, GVfs, or KIO, while macOS needs an MTP application such as OpenMTP.

Press B to cancel an export or stop a transfer server. Existing destination
files with the same name are replaced.

## Known limitations

These are known and do not need to be reported as issues.

- **Updates of Switch 1 games cannot be dumped.** The console only merges
  an update into its game while that game is running, and kaiHEN can only
  run KaiDump inside Switch 2 games. KaiDump then dumps the base version.
  This also applies to the Switch 1 version of Switch 2 Editions; their
  Switch 2 version usually comes complete with the update and is dumped as
  normal. It is a kaiHEN limitation that KaiDump cannot fix for now.
- **Updates of Switch 2 games** can only be dumped when kaiHEN was started
  through that game. Start kaiHEN through the game, then open KaiDump from
  the same hbmenu session.
- **Games that keep their data in the update** cannot be dumped when the
  update cannot be read, because the installed base version is incomplete on
  its own and there is nothing to fall back to. Examples are Hogwarts Legacy
  and Dave the Diver.
- **Some games only load their license while they are running.** They cannot
  be dumped unless kaiHEN was started through them, which is only possible
  for Switch 2 games.
- **DLC updates** are usually not included, for the same reason as game
  updates. The DLC is then dumped without its update.

KaiDump checks the installed update when you open a game. If it cannot be
read, the Game row and the confirmation screen say so before you start the
dump.

## Logs

KaiDump creates a new log each time it starts:

```text
sdmc:/switch/KaiDump/KaiDump.log
```

Please include this file when reporting a missing title, failed dump, or
transfer problem. It does not contain console keys or account information.

## Building

Start by installing
[devkitPro pacman](https://devkitpro.org/wiki/devkitPro_pacman). KaiDump needs
the following devkitPro packages:

- [`switch-dev`](https://devkitpro.org/wiki/Getting_Started) — the compiler,
  Switch tools, and standard libraries
- [`switch-sdl2`](https://github.com/devkitPro/pacman-packages/tree/master/switch/SDL2)
- [`switch-sdl2_ttf`](https://github.com/devkitPro/pacman-packages/tree/master/switch/SDL2_ttf)
- [`switch-libjpeg-turbo`](https://github.com/devkitPro/pacman-packages/tree/master/switch/libjpeg-turbo)
  — decodes the game icons

They can be installed with:

```sh
sudo dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-libjpeg-turbo
```

On systems where devkitPro uses the system `pacman`, use `pacman` in place of
`dkp-pacman`.

Next, install
[ounce-devkit](https://github.com/ouncebrew/ounce-devkit#install). Its installer
adds libounce, libkaihen, and the Switch 2 build files to the devkitPro setup.
Once everything is installed, build KaiDump with:

```sh
export DEVKITPRO=/opt/devkitpro
make
```

The resulting application is written to `KaiDump.nro`.

An overview of the source tree is available in
[`docs/architecture.md`](docs/architecture.md).

## Credits

KaiDump uses parts of
[OunceMTP](https://github.com/venustrikes/ouncemtp) for USB transfers. The
included code is covered by the ISC license in
[`licenses/OunceMTP-ISC.txt`](licenses/OunceMTP-ISC.txt).

Thanks to the kaiHEN, libounce, hbloader, and homebrew communities for making this project possible!
