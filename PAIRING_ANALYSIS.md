# Xbox 360 DVD Drive Pairing — Recovery (11775.3 Xenon) Analysis

## Summary

When the `11775.3_Xenon_Recovery` disc boots, the step you see as *"pairing the DVD
drive"* is the recovery app (`default.xex`) **making the console KeyVault and the DVD
drive (ODD) share the same 16‑byte DVD authentication key**. It never flashes firmware;
it just writes one key to two places:

1. **Console side** — the DVD key is **KeyVault key index `0x1A`** (16 bytes). The recovery
   calls `XeKeysSetKey(0x1A, key, 16)` and then `XeKeysSaveKeyVault`.
2. **Drive side** — it opens `\Device\CdRom0` and sends the drive a **vendor command**
   (`NtDeviceIoControlFile`, IOCTL `0x0004D014`) that unlocks and writes the same key into
   the drive.

After both halves hold the same key, the drive and console authenticate and the console
will talk to the drive. That is the whole of "pairing" — no firmware re-flash.

> This is drive⇔console authentication (interop/repair on hardware you own), **not** disc
> copy‑protection bypass and not unsigned‑code execution.

### Where the privilege boundary actually is (not an artificial kernel gate)

`XeKeys*` is exported by **both** the retail and devkit kernels (by ordinal), and the kernel
side is just a thin marshaller to the hypervisor — no `if(devkit)` branch, no
`XexCheckExecutablePrivilege` in the KeyVault path:

```c
XeKeysSetKey(index, buf, len)  ->  HvxKeysMarshal(0x36 /*54*/, index, 0, buf, len, ...)
XeKeysSaveKeyVault()           ->  HvxKeysMarshal(0x35 /*53*/, kvPhys, 0x4000, ...)
```

`HvxKeysMarshal` bundles the arguments into a physically‑contiguous buffer and traps into the
**hypervisor** (via the `Hvx*` ops). All privilege/policy enforcement is **HV‑side**, driven
by the caller XEX's signed system flags — the recovery is signed as **"Manufacturing Utility /
Manufacturing Support Tool"** (see `XexTool -l`), which is what authorizes the KeyVault write.

So it runs on:
- a **devkit / test kit** (your Falcon TK) — the devkit HV honors manufacturing‑flagged XEXs; or
- a **retail console whose HV checks are patched out** (RGH/JTAG) — the standard HV peek/poke +
  KeyVault/XeKeys allowances already neutralize this, so the same hypercalls succeed.

On a *stock* retail console the HV refuses it, because the privilege is cryptographically bound
to the XEX signature — it is **not** a software toggle you can flip. The ODD side
(`BB/30` over IOCTL `0x4D014`) is the drive accepting a vendor command and is not HV‑gated, so
that half works regardless; just confirm your patch also allows the KeyVault ops above.

## How it was found

- `default.xex` / `xam.xex` / `xbdm.xex` are devkit XEX2 (zero AES key). Basefiles dumped
  with xorloser `XexTool -b`; import names via `XexTool -i` + `x360_imports.idc`.
- Loaded `default.bin` into IDA 9.4 (PPC) headless; Hex‑Rays PPC decompiler.
- The recovery app has **no PDB** on the symbol server (`recappext.pdb` is absent), so
  functions are unnamed `sub_*`. Kernel call signatures were confirmed from
  `xboxkrnld.exe.i64` in the symbol server.

Key strings (in `default.bin` `.rdata`) and the functions referencing them:

| String | Addr | Function |
|---|---|---|
| `Resetting ODD key` | `0x920171F4` | orchestrator `sub_92089638` |
| `Failed to set DVD Key in KeyVault with status %x` | `0x920171C0` | same |
| `Failed to set DVD Key in the ODD with status %x` | `0x9201718C` | same |
| `Failed to unlock DVD Key in the ODD with status %x` | `0x92017120` | `sub_920894C8` / `sub_920D7B80` |
| `Didn't find a DVD Drive.  Recovery will continue` | `0x920170EC` | ODD open path |

## The orchestrator (`sub_92089638`)

Runs a sequence of steps, each gated by a bit in a flags word. The DVD‑relevant step
("Resetting ODD key"):

```
key = <16 random bytes>; validate; retry until valid     ; XeKeysGenerateRandomKey + check
XeKeysSetKey(0x1A, key, 16)                               ; "set DVD Key in KeyVault"
  -> on failure: print "Failed to set DVD Key in KeyVault"
write key into the drive  (sub_920894C8 or sub_92089088)  ; "set DVD Key in the ODD"
  -> on failure: print "Failed to set DVD Key in the ODD", return 0xC00000A3
... later: XeKeysSaveKeyVault / write KeyVault blob back to flash
```

KeyVault indices seen nearby: `0x16` = game region (`Resetting devkit game region`,
value `0x7FFF`), `0x36` = console certificate (`Repairing console certificate`, 424 bytes),
`0x1A` = **DVD key** (16 bytes).

Kernel signatures (from `xboxkrnld.exe.i64`):

```c
int XeKeysSetKey(int index, void* buffer, int length);          // HvxKeysMarshal(54, index,0,buf,len,...)
int XeKeysGetKey(int index /* ... */);                          // HvxKeysMarshal(55, ...)
int XeKeysGetKeyProperties(...);                                // returns key length etc.
int XeKeysSaveKeyVault(...);
```

## The ODD (drive) interface

All drive traffic goes to **`\Device\CdRom0`** opened with `NtOpenFile`
(`DesiredAccess=0x80100000`, `ShareAccess=1`, `OpenOptions=0x20`), then
`NtDeviceIoControlFile` with **IOCTL `0x0004D014`**.

The low‑level senders are `sub_920D7A40` (command→drive) and `sub_920D7AC8`
(command + read←drive). Both build a **36‑byte request** that points at a **command block**:

### IOCTL `0x4D014` request structure (36 bytes)

| Off | Type | Value / meaning |
|----:|------|-----------------|
| 0x00 | u16 | `0x0024` (= 36, size of this request) |
| 0x02 | u16 | 0 |
| 0x04 | u32 | 0 |
| 0x07 | u8  | direction: `0` = data **to** drive, `1` = data **from** drive |
| 0x08 | u32 | transfer length = `commandBlock.size + 2` |
| 0x0C | u32 | 0 |
| 0x10 | u32 | pointer to the **command block** (below) |
| 0x14 | u8  | `0x55` ('U') — signature/flag |
| 0x1B | u16 | `commandBlock.size + 2` (copy) |
| 0x20 | u32 | 0 |

(Bytes not listed are zero. Offsets 0x02–0x06/0x0C/0x20 observed constant‑zero.)

### Command block

| Off | Type | Meaning |
|----:|------|---------|
| 0x00 | u16 | total size of the block (`56` for BB/30, `24` for A5/10) |
| 0x02..0x07 | — | reserved (0) |
| 0x08 | u8  | **opcode** |
| 0x09 | u8  | **modifier / sub‑command** |
| 0x0A.. | bytes | payload (e.g. the 16‑byte key) |

### Vendor commands used for pairing

| Opcode | Mod | Dir | Payload | Purpose |
|:------:|:---:|:---:|---------|---------|
| `0xBB` | `0x30` | to drive | 16 × `0xFF` | **unlock** the drive's key slot |
| `0xBB` | `0x30` | to drive | 16‑byte key | **set DVD key** in the drive |
| `0xA5` | `0x10` | to drive | 16‑byte key | authenticate with a key |
| `0x25` | — | from drive | reads 16 bytes | read drive's current key / identity |

> Note: the top 2 bits of the opcode byte are flag bits (`opcode & 0x40` is preserved,
> read path ORs `0xC0`). For the pairing commands above use the opcodes verbatim.

## "Old vs new" — what the recovery actually branches on

There are **two independent axes**. Neither is "the drive brand."

### Axis 1 — console / motherboard generation (picks the write routine)

In the orchestrator:

```
flags = XboxHardwareInfo->Flags;        // kernel import, ordinal 0x156, at 0x92000518
if ((flags & 0xF0000000) >= 0x40000000) // newer boards
     sub_92089088(key);                 // "new" writer  (INQUIRY-aware)
else                                     // older boards (incl. your Falcon)
     sub_920894C8(key);                 // "old" writer  (plain BB/30, no detection)
```

So "old vs new" is **the console**, read from `XboxHardwareInfo.Flags` top nibble, not the
drive. `< 0x40000000` = older boards (Xenon / Zephyr / **Falcon** / Opus and many Jasper);
`>= 0x40000000` = later boards.

- **Old-console writer `sub_920894C8`:** open `\Device\CdRom0`, then *unconditionally*
  `BB/30` + `0xFF` (unlock) then `BB/30` + key (set). **No INQUIRY, no per-drive logic — the
  same sequence for every drive brand/model.** This is the path your Falcon uses.
- **New-console writer `sub_92089088`:** runs an INQUIRY first (below) and only diverges for
  one specific drive.

### Axis 2 — drive model (only ONE model is special-cased)

`sub_920D7CA8` sends a standard SCSI **INQUIRY (0x12)**, reads 36 bytes, and compares the
28-byte vendor/product/revision field against a table (`off_9201D828`) that has **exactly two
entries**, both Lite-On DG-16D4S:

```
"PLDS    DG-16D4S        J504"
"PLDS    DG-16D4S        9504"
```

`sub_92089088` then does:

```
if (drive is NOT a DG-16D4S J504/9504)
     BB/30 unlock+set   (same as everything else)
else { // DG-16D4S
     cur = read drive key (vendor 0x25);
     if (cur == {16x 0xFF})             // blank / factory drive
          BB/30 set  key = CONST_A;     // CONST_A/B are FIXED test constants, below
          A5/10 authenticate CONST_B;
     else                               // drive already keyed
          XeKeysSetKey(0x1A, CONST_A);  // only sync the KeyVault, don't rewrite drive
}
```

The DG-16D4S branch writes **fixed Microsoft test constants** (from `byte_92017040`), not a
per-console key — it is a manufacturing/test provisioning flow, not general pairing:

```
byte_92017040 +0x00: FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF FF  (blank marker)
              +0x10: 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FA
              +0x20: FA EE DD CC BB AA 99 88 77 66 55 44 33 22 11 00  (CONST_A)
              +0x30: 64 7F 45 E8 6B 7C 2B E9 36 CB 0F 0B 92 64 45 B5  (CONST_B)
```

### What this means for your stack of drives

- **Brand doesn't matter.** `BB`, `A5`, `0x25` are the Microsoft-mandated vendor commands
  that *every* licensed 360 drive implements, and the command block is passed straight through
  (`\Device\CdRom0`, IOCTL 0x4D014). Samsung MS25/MS28, BenQ VAD6038, Hitachi-LG GDR-3120L
  (all revs) and Lite-On DG-16D2S all take the identical `BB/30` unlock+set.
- **On your Falcon specifically, so does the DG-16D4S** — the Falcon is an "old-console"
  board, so the recovery uses `sub_920894C8` (plain `BB/30`) for *every* drive, including a
  DG-16D4S. The special D4S flow above only runs on newer boards.
- **Coverage limit:** this 2010 recovery predates the Lite-On **DG-16D5S** (slim). There is no
  table entry for it and no handling; D5S key/unlock is a different, harder process this SDK
  does not implement. So "all drives" realistically means everything up to and including
  DG-16D4S.

So: to pair any drive to your Falcon test kit, the plain `BB/30` unlock+set (what
`sub_920894C8` does) is the correct, universal path. The code exposes INQUIRY detection too,
so the tool can name the drive and warn on the D4S/D5S edge cases.

### Drive INQUIRY strings the kernel recognises

`ClassifyDrive()` matches the exact vendor/product/revision strings from this build's kernel
`SataCdRom` driver (`xboxkrnld.exe` 4C6787B8). The recognised-drive table is at `0x80044C28`
(records of `{vendorPtr, productPtr, firmwarePtr, flags}`); Lite-On is matched inline in
`SataCdRomInitialize` via `SataCdRomVendorPLDS`/`SataCdRomRevisionPLDS7485`/`...8385`:

The 11775.3 kernel only lists Hitachi `0059`/`0061` and Samsung `ms25`/`ms28` in its table,
plus Lite-On `7485`/`8385` matched inline. The **complete** set below is verified against the
newer SDK kernel `../21256.18_Xenon_Recovery_with_Symbols` (`xboxkrnlc` 55EFF578), whose
drive-match table stores full 28-char records (vendor 8 + product 16 + revision 4):

| Family | INQUIRY vendor (8) | INQUIRY product (16) | Revisions | Source |
|---|---|---|---|---|
| Hitachi-LG | `HL-DT-ST` | `DVD-ROM GDR3120L` | `0046` `0047` `0058` `0059` `0078` `0079` | 21256.18 kernel |
| Samsung    | `TSSTcorp` | `DVD-ROM TS-H943A` | `ms25` `ms28` | both kernels |
| BenQ       | `PBDS␣␣␣␣` | `VAD6038-64930C␣␣` | (none) | 21256.18 kernel |
| Lite-On D2S| `PLDS␣␣␣␣` | `DG-16D2S␣␣␣␣␣␣␣␣` | `7485` `8385` `9345` | both kernels |
| Lite-On D4S| `PLDS␣␣␣␣` | `DG-16D4S␣␣␣␣␣␣␣␣` | `9504`, `J504` | 21256.18 kernel + 11775.3 recovery |
| BenQ (6243)| `SATA␣␣␣␣` | `DVD-ROM␣␣␣␣␣␣␣␣␣` | `6243` | 21256.18 kernel — confirmed on HW |

Vendor fields are fixed 8 bytes, space-padded. **BenQ and Lite-On are different vendors:
`PBDS` vs `PLDS`.** Hitachi/Samsung carry the `DVD-ROM ` prefix in the product field; Lite-On
and BenQ put the model there directly.

**DG-16D5S is deliberately excluded** — its flash chip isn't writable, so the DVD key can't be
set in the drive at all; detection would only ever report "can't pair."

### Unrecognized firmware → you choose the method

`Odd_DetectDrive` also sets `KnownFirmware` (1 if the revision is in the known-good list for
that family — the unions above). When the firmware revision is **not** recognized,
`DvdPair_PairConnectedDrive` (AUTO) does **nothing** and returns
`DVDP_STATUS_UNKNOWN_FIRMWARE`, so the front-end can ask which key-set sequence to try and
re-call `DvdPair_PairConnectedDriveEx(key, writeKV, method)` with:

- `DVDP_ODD_OLD` — the old path: `BB/30` unlock (`0xFF`) + `BB/30` set key. (What
  `sub_920894C8` does; works for every pre‑D4S drive.)
- `DVDP_ODD_NEW` — the new path: `BB/30` unlock + `BB/30` set key **+ `A5/10` authenticate**
  with the key (the extra handshake `sub_92089088` uses on newer drives/boards).

An explicit `DVDP_ODD_OLD`/`_NEW` overrides detection entirely (including for a D4S), so you
can force either sequence on any drive. `DVDP_ODD_AUTO` keeps mirroring the recovery for
recognized firmware.

## Reproducing it from the dashboard

See `DvdPair.h` / `DvdPair.cpp`. The important realisation for *swapping drives for test*:
you usually do **not** need a brand‑new random key. You have two clean options:

- **Adopt console → drive:** read the console's existing KeyVault DVD key
  (`XeKeysGetKey(0x1A)`) and write it into the freshly‑swapped drive with `BB/30`. The drive
  now matches the console. (Recommended for swapping test drives.)
- **Full re‑pair (what recovery does):** generate a fresh 16‑byte key, write it to both the
  KeyVault (`XeKeysSetKey(0x1A)` + `XeKeysSaveKeyVault`) and the drive.

**Back up your KeyVault first.** Writing key `0x1A` and `XeKeysSaveKeyVault` persists to NAND.
