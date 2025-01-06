# Visual Copy
*Lightweight utility that highlights the application that successfully copied data into clipboard*

Reimplementation of [Kevin Gosse](https://x.com/KooKiz/)'s original idea
for his [ClipPing](https://github.com/kevingosse/ClipPing) application.

This is a pure Win32 application that adds extra animation and optionally a sound effect to the active window
whenever clipboard content changes.

## Features

* Minimal footprint
* Multiple different animations and settings
* Customizable effect color
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
* Fix Debug builds.
