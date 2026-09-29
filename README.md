# VendorPlus - Every Vendor from Artifact Formation

A mod for CONTROL Resonant: buy the materials an artifact needs without leaving Artifact Formation, from any vendor in the game.

Download and install instructions are on Nexus Mods (search for VendorPlus in the CONTROL Resonant section). This repository is the full source.

## Features

- A tab row like the game menu's on Artifact Formation, the vendor and the wardrobe: switch with LB / RB, the keyboard's menu keys, or a click.
- Arrows in the vendor's title bar (R / T, or the right stick on a controller) flip through all 7 vendors, each named by its zone. On the vendor screen Q / E and LT / RT stay the vendor's amount keys.
- Each vendor keeps its own zone's vendor level.
- A vendor stays locked until you've visited it once at its counter.
- The Wardrobe tab opens the game's own wardrobe with a wardrobe's camera: the full body, and a close-up for the face slots (learned from the wardrobes you use).

## Requirements (to play)

- [f2g DLL Mod Loader (crloader)](https://www.nexusmods.com/controlresonant/mods/9)

## Building

Requires Windows and the Visual Studio 2022 Build Tools (C++ workload). Run `build.bat`; it builds `build\vendorplus.dll`. To install your build, put it in `crmods\VendorPlus` in the game folder together with the files in `mod\`.

## How it works

`vendorplus.dll` is loaded by [f2g DLL Mod Loader (crloader)](https://www.nexusmods.com/controlresonant/mods/9) from `crmods\VendorPlus`. At start-up it reads the game executable from disk, finds the game functions it needs by byte signatures, and installs a few inline hooks inside the game process only. `VendorPlus.js` is appended to the game's UI bundle when the game loads it, and talks to the DLL through a `coui://` endpoint. There is no network code; the mod writes only its own log and settings files in its own folder.

## Antivirus false positives

Some antivirus tools, including Windows Defender (`Trojan:Win32/Wacatac.C!ml`), may flag the DLL because it is unsigned and hooks into the game. This is a false positive and has been reported to Microsoft. The full source is here, so you can check it or build the DLL yourself with `build.bat`.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

## Credits

- **fame2gin** for f2g DLL Mod Loader.
- **kkyleeb21** for MapFusion, which showed how to add scripts to the game's UI.
- **Eurogamer** for the vendor locations guide.

## License

MIT, see [LICENSE](LICENSE). CONTROL Resonant is a game by Remedy Entertainment; this project isn't affiliated with or endorsed by Remedy.
