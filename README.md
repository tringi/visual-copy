# Visual Copy
*Highlight active window after successfull copy of data into clipboard and find mouse on shake*

Initially a reimplementation of [Kevin Gosse](https://x.com/KooKiz/)'s original idea
for his [ClipPing](https://github.com/kevingosse/ClipPing) application as a pure Win32 application.
Includes optional sound effect, and additionally can highlight mouse cursor location after shaking the mouse.

![](https://github.com/tringi/visual-copy/raw/refs/heads/main/Example.gif)

*Sorry for the poor quality and slowed-down GIF. See [Example.mp4](https://github.com/tringi/visual-copy/raw/refs/heads/main/Example.mp4) or...*

Find precompiled EXE's in [/Bin](/Bin)

## Features

* Minimal footprint
* Multiple different animations and settings
* Customizable effect color
* Customizable find mouse effect
* Optional audio effect

## Command line parameters

* `-hidden` - starts the program without notification (tray) icon
* `-terminate` - exits all already running instances (for all users if run as admin)

## Minimal requirements

* Windows Vista
* Memory usage typically peaks at about 8 MB of RAM

## Recommended

* Windows 8, 32-bit colors, with DWM running

## TODO:

* Create good icon. Ideally in style that'd fit also Windows 7 and 8.
* Figure out why are artifacts sometimes left on Windows Vista/7 in 16-bit mode with Aero off.
