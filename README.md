# DVD Drive Pairing Tool

A text-mode Xbox 360 app (ATG console sample style, like the Simple 360 NAND Flasher) that pairs
a DVD drive to the console, so drives can be swapped between consoles without reinstalling the OS
or reflashing. It reproduces the pairing step of the 11775.3 recovery; see
[PAIRING_ANALYSIS.md](PAIRING_ANALYSIS.md) for how that works.

**Not yet confirmed working on hardware.** Parts of the drive interface are reconstructed (flagged
in `DvdPair.cpp`). Back up your KeyVault before using it.

Runs on a devkit/test kit, or on a retail console with a patched hypervisor (RGH/JTAG).

## Controls

- **A** — pair drive to this console (writes the console's current KeyVault DVD key into the
  drive; KeyVault untouched). On an unrecognized firmware it asks X=OLD / Y=NEW.
- **X** — read and print the drive's current key.
- **Y** — full re-pair: generate a new key, write to KeyVault **and** drive (back up first).
- **BACK** — exit (reboot).

## Files

- `main.cpp` — console UI (detect drive, pair, read key, full re-pair).
- `DvdPair.cpp/.h` — the pairing logic.
- `OutputConsole.cpp/.h` — thin wrapper over `ATG::Console` (`dprintf`).
- `atg/Common/` — ATG framework (headers, sources, `Release_LTCG/AtgFramework.lib`); only the
  three changed files are tracked.
- `AtgFramework-installer.bat` — copies missing ATG files from the SDK and builds the lib.
- `tools/XexTool.exe` — vendored xorloser XexTool.
- `font.xpr` — console font, **embedded into the XEX**, not shipped separately.
- `xex.xml` — XEX privileges + compression config for `imagexex`.
- `build.bat` — the build.

## Building

You need the Xbox 360 SDK (XDK) and the ATG framework; no Visual Studio is needed. `build.bat`
uses `%XEDK%` or defaults to the SDK's standard install location. Override `%ATG_DIR%`
(must have `Common\*.h` + `Common\Release_LTCG\AtgFramework.lib`) or `%XEXTOOL%` to use your own
copies.

Run `AtgFramework-installer.bat` once before the first build. It copies the ATG sources from the
SDK and builds `AtgFramework.lib`. The repo only tracks the three ATG files this tool needs
changed (`AtgConsole.cpp`, `AtgConsole.h` and `AtgResource.cpp`, from the Simple 360 NAND
Flasher), and the installer never overwrites existing files. To rebuild the lib, delete it and
run the installer again.

```
build.bat
```

This is a debug build: our code is unoptimized, with symbols in `bin\PairDvd.pdb`. ATG and the
SDK libraries stay release. Use `build.bat release` for an optimized build without symbols.

Steps: `cl` compile -> `link /XEX:NO` -> `imagexex /section:font=font.xpr,RO`
(`PairDvd_img.xex`) -> `XexTool -r a -m d -e u -c u` -> **`bin\PairDvd.xex`**.

The final XEX is **devkit, unencrypted, uncompressed, with all restrictions removed**
(media/region/bounding path/console-id/dates/keyvault-privilege/signed-keyvault/library-
version/revocation/media-id). Copy it to the console and launch it by path; the console only
auto-launches files named `default.xex`.

## How the font is embedded

Same as the Simple 360 NAND Flasher: `imagexex /section:font=font.xpr,RO` embeds `font.xpr` as a
XEX resource, and `AtgResource`'s `embed:\font` loads it with `XGetModuleSection(module, "font")`.
The link step must use `/XEX:NO`; otherwise `link` outputs a finished XEX as the `.exe` and
`imagexex` ignores `/section`.

To check a build, `XexTool -l bin\PairDvd.xex` should list `font` under Resources, and
`XexTool -d <dir> bin\PairDvd.xex` should dump a file identical to `font.xpr`.
