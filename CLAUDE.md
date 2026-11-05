# CLAUDE.md — DVD drive pairing tool

Context and decisions for this project, so future sessions don't re-derive them.

## What this is

A text-console XEX that reimplements the Xbox 360 recovery's "pair the DVD drive" step, so
drives can be swapped between consoles for testing **without reinstalling the OS or
reflashing**. Hardware target: Xbox 360 **Falcon test kit**, or retail with a patched HV.

Scope is explicitly drive⇔console *authentication* pairing (interop/repair on owned hardware).
Not disc copy protection, not unsigned-code execution.

## Build

`build.bat` uses the SDK command-line tools, **no Visual Studio**, and outputs
`bin/PairDvd.xex`. From PowerShell: `& cmd /c ".\build.bat"`.

- **Configurations:** debug is the default (`/Zi /Od`, `/DEBUG`, symbols in `bin\PairDvd.pdb`);
  `build.bat release` is optimized with no symbols. Debug keeps `NDEBUG` and `/LTCG` on because
  the prebuilt `AtgFramework.lib` and the SDK libs it links are release builds; don't define
  `_DEBUG`.

**Ground truth:** the SDK (`%XEDK%`) and the Simple 360 NAND Flasher (its source, plus a built
copy and build logs). When the build misbehaves, compare against the flasher before concluding
a tool is broken.

- **Toolchain:** `%XEDK%\bin\win32\{cl,link,imagexex}.exe` (cl is the PowerPC cross-compiler,
  v16 / VS2010). Use the **xbox** include/lib dirs only; `include\win32`'s `xnamath.h` rejects
  Xbox 360.
- **Vendored:** XexTool (`tools`; override with `%XEXTOOL%`). Link the same lib set the flasher
  uses.
- **ATG framework:** `atg/Common` (override with `%ATG_DIR%`) is git-ignored except
  `AtgConsole.cpp`/`.h` and `AtgResource.cpp`, which differ from stock (the flasher's changes).
  Stock `AtgConsole` has no `Display()` and stock `AtgResource.cpp` can't load `embed:\font`.
  `AtgFramework-installer.bat` copies missing files from the SDK, never overwriting, then builds
  `AtgFramework.lib` if it's missing (the SDK has no prebuilt lib), using the settings from the
  Release_LTCG configuration of the SDK's `AtgFramework2010.vcxproj`.
- **Font:** embedded the way the flasher does it: `link /XEX:NO`, then
  `imagexex /CONFIG:xex.xml /section:font=font.xpr,RO`. At runtime `embed:\font` =
  `XGetModuleSection(module,"font")`. The switch is the flasher's
  `<AdditionalSections>font=...,RO`; neither project's `xex.xml` has a `<section>` element.
  - **`/XEX:NO` is critical.** Without it `link` writes a finished XEX to the `.exe` (magic
    `XEX2`, with the PE as `.pe`), and `imagexex /section` silently embeds nothing when given
    that as `/IN`. Don't conclude from this that `imagexex` can't embed sections.
  - **Verify with XexTool, not grep** (the intermediate XEX is compressed): `XexTool -l` must
    list `font` under Resources, and `XexTool -d <dir>` must dump a file identical to `font.xpr`.
- **Post-process:** `XexTool -r a -m d -e u -c u` removes all XEX restrictions and forces the
  output to **devkit, unencrypted, uncompressed**. The `font` resource survives this.
- **Launching:** the 360 only auto-launches `default.xex`, so `PairDvd.xex` must be launched by
  path. Set `NAME=Default` in `build.bat` for folder auto-launch.
- **NT/XeKeys APIs:** `DvdPair.cpp` declares them itself (C linkage) because `<xtl.h>` doesn't
  declare `NtOpenFile`/`ANSI_STRING`/`XboxHardwareInfo`. They resolve from `xboxkrnl.lib` by
  name, and the local struct layouts match the kernel's.

## Key findings (from the binaries)

- **Pairing = one 16-byte DVD key written to two places:** the console KeyVault and the drive.
- **KeyVault side:** the DVD key is **KeyVault index `0x1A`**. `XeKeysSetKey(0x1A, key, 16)` then
  `XeKeysSaveKeyVault`. (`XeKeysSetKey` -> `HvxKeysMarshal(0x36,...)`, `XeKeysSaveKeyVault` ->
  `HvxKeysMarshal(0x35,...)`. Index 0x16 = game region, 0x36 = cert.)
- **Drive side:** open `\Device\CdRom0` (`NtOpenFile`, access `0x80100000`, share 1, openopt
  `0x20`); vendor commands via `NtDeviceIoControlFile`, **IOCTL `0x0004D014`**.
  - Command block: `[u16 size][6 rsvd][opcode][modifier][payload...]`.
  - `BB/30` + 16×`0xFF` = unlock; `BB/30` + key = set key; `A5/10` + key = authenticate;
    `0x25` = read current 16-byte drive key/id.
  - The IOCTL request is 36 bytes wrapping a pointer to the command block (layout in
    `PAIRING_ANALYSIS.md`).

## Decisions

1. **"Old vs new" is the console, not the drive.** Gate is
   `XboxHardwareInfo.Flags & 0xF0000000 >= 0x40000000` -> new writer `sub_92089088`, else old
   writer `sub_920894C8`. The Falcon is "old" and uses plain `BB/30` for *every* drive. The
   only drive-model special case is the Lite-On **DG-16D4S**, only on new boards, and it writes
   *fixed Microsoft test constants*, so it's provisioning rather than general pairing.
2. **Universal command set.** `BB`/`A5`/`0x25` are MS-mandated vendor commands common to all
   licensed 360 drives, and the IOCTL is raw passthrough. Drive *brand* only affects
   detection/reporting.
3. **Drive identification uses the kernel's exact INQUIRY strings** (table in
   `PAIRING_ANALYSIS.md`): Hitachi `HL-DT-ST`/`DVD-ROM GDR3120L`, Samsung
   `TSSTcorp`/`DVD-ROM TS-H943A`, BenQ `PBDS`/`VAD6038-64930C`, Lite-On `PLDS`/`DG-16D2S` &
   `DG-16D4S`. **BenQ is `PBDS`, Lite-On is `PLDS`: one letter apart, kept distinct.**
4. **BenQ fw-6243 units report as `SATA`/`DVD-ROM`** on real hardware (user-confirmed), so they're
   matched explicitly as BenQ instead of UNKNOWN.
5. **DG-16D5S is intentionally unsupported**: its flash isn't writable, so the key can't be set.
   It's detected and refused up front (`0xC00000A3`) rather than failing at the write.
6. **Unrecognized firmware is not guessed.** `Odd_DetectDrive` sets `KnownFirmware`; the AUTO
   path returns `DVDP_STATUS_UNKNOWN_FIRMWARE` (writes nothing) so the UI can ask the user to
   pick `DVDP_ODD_OLD` (BB/30 unlock+set) or `DVDP_ODD_NEW` (+ A5/10 authenticate) and re-call
   `DvdPair_PairConnectedDriveEx`. Explicit OLD/NEW overrides detection.
7. **The privilege boundary is the hypervisor, not a kernel gate.** `XeKeys*` are exported on
   retail and devkit alike, and the kernel is a thin marshaller to the HV. Enforcement is
   HV-side, keyed off the XEX's signed system flags (the recovery is "Manufacturing
   Utility/Support Tool"). The tool runs on a devkit/testkit, or on retail with a patched HV
   (RGH/JTAG) that neutralizes those checks. The user runs such a patch.

## Caveats / open items

- **Not yet confirmed working on hardware.** An earlier build, which had no embedded font,
  crashed right at launch. The font is now embedded, but the fix hasn't been tested on a console.
  If it still crashes, suspect `Odd_DetectDrive`, which runs at startup before the menu.
- **Reconstructed code:** opcodes, device path, IOCTL code and KeyVault calls come from the
  binaries, but a few reserved bytes of the IOCTL request struct and the INQUIRY read-command
  passthrough layout are reconstructed (flagged inline in `DvdPair.cpp`). Validate on hardware.
- `DVDP_ODD_NEW` authenticates (`A5/10`) with the *same* key it sets. The recovery's D4S flow
  authenticates with a separate constant; add an auth-key parameter if that's ever needed.
- On a patched-HV retail box, confirm the KeyVault is the real writable one before relying on
  `XeKeysSaveKeyVault` (a synthetic KV may persist elsewhere). The drive-side write is unaffected.

## Reference sources (not in this repo)

- `11775.3_Xenon_Recovery` (from the 11775.3 TK SDK): the recovery analyzed (`default.xex`, no
  PDB). Its kernel is `xboxkrnld.exe` build `4C6787B8`: `XeKeysSetKey`, keyvault marshal,
  SataCdRom table @`0x80044C28`, `SataCdRomVendorPLDS`/`...Revision7485`/`8385`.
- `21256.18_Xenon_Recovery_with_Symbols`: newer kernel `xboxkrnlc.exe` build
  `55EFF578`, with the complete drive-match table (BenQ `PBDS`/`VAD6038`, full
  Hitachi/Samsung/Lite-On revs, `SATA`/`DVD-ROM`/`6243`).
- Method: basefile dumped with xorloser `XexTool v6.6 -b` (devkit zero-key), imports named via
  `XexTool -i` + `x360_imports.idc`, disassembled with IDA 9.4 Pro headless
  (`idat.exe -A -S<script>`) and the Hex-Rays PPC decompiler.

## Conventions

- Keep findings traceable: when a constant/string comes from a binary, cite the source
  (`[kernel]`/`[recovery]`/build id) in a comment, as done in `ClassifyDrive`.
- Git commit messages end with a `Co-Authored-By: <model name> <noreply@anthropic.com>` trailer
  naming the model currently in use (e.g. `Claude Opus 5.5`).
