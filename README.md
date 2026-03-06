<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# <p align="center">KosPS4 Emulator</p>

<h1 align="center">
 <a href="https://discord.gg/bFJxfftGW6">
        <img src="https://img.shields.io/discord/1080089157554155590?color=5865F2&label=shadPS4%20Discord&logo=Discord&logoColor=white" width="275">
 <a href="https://github.com/shadps4-emu/shadPS4/releases/latest">
        <img src="https://img.shields.io/github/downloads/shadps4-emu/shadPS4/total.svg" width="140">
 <a href="https://shadps4.net/">
        <img src="https://img.shields.io/badge/shadPS4-website-8A2BE2" width="150">
 <a href="https://x.com/shadps4">
        <img src="https://img.shields.io/badge/-Join%20us-black?logo=X&logoColor=white" width="100">
</h1>

<p align="center">"Ahh, Kos...  
or some say, Kosm...  
Do you hear our prayers?      
Grant us eyes, grant us eyes!"</p>

### <p align="center">*— Micolash, Host of the Nightmare*</p>

# General information
**KosPS4** is an early Bloodborne optimized fork of the ShadPS4 v14.0 **PlayStation 4** emulator for **Windows**, **Linux** and **macOS** written in C++. 
Right now KosPS4 is planned as Windows only.
The goal is a stable "complete" 60fps 1080p+ experience with enhanced graphics architecture to integrate [**fromsoftserve's BB PC Remaster Mod**](https://www.nexusmods.com/bloodborne/mods/45) without bottlenecks.

> [!IMPORTANT]
> This is the emulator core for use with BB Launcher, which does not include a GUI.


# Status

> [!IMPORTANT]
> KosPS4 is early in development, don't expect a flawless experience. As of 2026-03-06 the fork is functioning with reduced memory leaks and significant FPS stability improvements but there are many bugs and shader issues to be addressed in the coming commits.

# Why

After following the ShadPS4 project closely since release, I have felt there is an opportunity to create a personal fork that drills into performance and optimizations specifially around Bloodborne. 
All credit goes to the ShadPS4 team of course, I am simply interested in creating the smoothest BB emulation experience possible.

# Building (ShadPS4 Legacy Docs)

## Windows

Check the build instructions for [**Windows**](https://github.com/shadps4-emu/shadPS4/blob/main/documents/building-windows.md).

# Usage examples

To get the list of all available commands and also a more detailed description of what each command does, please refer to the `--help` flag's output.

Below is a list of commonly used command patterns:
```sh
shadPS4 CUSA00001 # Searches for a game folder called CUSA00001 in the list of game install folders, and boots it.
shadPS4 --fullscreen true --config-clean CUSA00001    # the game argument is always the last one,
shadPS4 -g CUSA00001 --fullscreen true --config-clean # ...unless manually specified otherwise.
shadPS4 /path/to/game.elf # Boots a PS4 ELF file directly. Useful if you want to boot an executable that is not named eboot.bin.
shadPS4 CUSA00001 -- -flag1 -flag2 # Passes '-flag1' and '-flag2' to the game executable in argv.
```

# Debugging and reporting issues

For more information on how to test, debug and report issues with the emulator or games, read the [**Debugging documentation**](https://github.com/shadps4-emu/shadPS4/blob/main/documents/Debugging/Debugging.md).

# Keyboard and Mouse Mappings

> [!NOTE]
> Some keyboards may also require you to hold the Fn key to use the F\* keys. Mac users should use the Command key instead of Control, and need to use Command+F11 for full screen to avoid conflicting with system key bindings.

| Button | Function |
|-------------|-------------|
F10 | FPS Counter
Ctrl+F10 | Video Debug Info
F11 | Fullscreen
F12 | Trigger RenderDoc Capture

> [!NOTE]
> Xbox and DualShock controllers work out of the box.

| Controller button | Keyboard equivalent |
|-------------|-------------|
LEFT AXIS UP | W |
LEFT AXIS DOWN | S |
LEFT AXIS LEFT | A |
LEFT AXIS RIGHT | D |
RIGHT AXIS UP | I |
RIGHT AXIS DOWN | K |
RIGHT AXIS LEFT | J |
RIGHT AXIS RIGHT | L |
TRIANGLE | Numpad 8 or C |
CIRCLE | Numpad 6 or B |
CROSS | Numpad 2 or N |
SQUARE | Numpad 4 or V |
PAD UP | UP |
PAD DOWN | DOWN |
PAD LEFT | LEFT |
PAD RIGHT | RIGHT |
OPTIONS | RETURN |
BACK BUTTON / TOUCH PAD | SPACE |
L1 | Q |
R1 | U |
L2 | E |
R2 | O |
L3 | X |
R3 | M |

Keyboard and mouse inputs can be customized in the settings menu by clicking the Controller button, and further details and help on controls are  also found there. Custom bindings are saved per-game. Inputs support up to three keys per binding, mouse buttons, mouse movement mapped to joystick input, and more.


# Firmware files (may or may not be needed)

shadPS4 can load some PlayStation 4 firmware files.
The following firmware modules are supported and must be placed in shadPS4's `sys_modules` folder.

<div align="center">

| Modules                 | Modules                 | Modules                 | Modules                 |  
|-------------------------|-------------------------|-------------------------|-------------------------|  
| libSceCesCs.sprx        | libSceFont.sprx         | libSceFontFt.sprx       | libSceFreeTypeOt.sprx   |
| libSceJpegDec.sprx      | libSceJpegEnc.sprx      | libSceJson.sprx         | libSceJson2.sprx        |  
| libSceLibcInternal.sprx | libSceNgs2.sprx         | libScePngEnc.sprx       | libSceRtc.sprx          |
| libSceUlt.sprx          | libSceAudiodec.sprx     |                         |                         |
</div>

> [!Caution]
> The above modules are required to run the games properly and must be dumped from your legally owned PlayStation 4 console.



# Main team

- [**georgemoralis**](https://github.com/georgemoralis)
- [**psucien**](https://github.com/psucien)
- [**viniciuslrangel**](https://github.com/viniciuslrangel)
- [**roamic**](https://github.com/roamic)
- [**squidbus**](https://github.com/squidbus)
- [**frodo**](https://github.com/baggins183)
- [**Stephen Miller**](https://github.com/StevenMiller123)
- [**kalaposfos13**](https://github.com/kalaposfos13)

Logo is done by [**Xphalnos**](https://github.com/Xphalnos)

# Contributing

If you want to contribute, please read the [**CONTRIBUTING.md**](https://github.com/shadps4-emu/shadPS4/blob/main/CONTRIBUTING.md) file.\
Open a PR and we'll check it :)


# Special Thanks

A few noteworthy teams/projects who've helped us along the way are:

- [**Panda3DS**](https://github.com/wheremyfoodat/Panda3DS): A multiplatform 3DS emulator from our co-author wheremyfoodat. They have been incredibly helpful in understanding and solving problems that came up from natively executing the x64 code of PS4 binaries

- [**fpPS4**](https://github.com/red-prig/fpPS4): The fpPS4 team has assisted massively with understanding some of the more complex parts of the PS4 operating system and libraries, by helping with reverse engineering work and research.

- **yuzu**: Our shader compiler has been designed with yuzu's Hades compiler as a blueprint. This allowed us to focus on the challenges of emulating a modern AMD GPU while having a high-quality optimizing shader compiler implementation as a base.

- [**felix86**](https://github.com/OFFTKP/felix86): A new x86-64 → RISC-V Linux userspace emulator

# License

- [**GPL-2.0 license**](https://github.com/shadps4-emu/shadPS4/blob/main/LICENSE)
