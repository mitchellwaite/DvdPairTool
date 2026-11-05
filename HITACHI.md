# Hitachi-LG GDR3120L: notes and a potential fix

Status: **not fixed, not tested.** Nothing below has been implemented. Treat Hitachi pairing as
risky until it's tested on drives that can be lost.

## What happened

A Hitachi-LG GDR3120L (rev `0047`, Falcon test kit, kernel 2.0.17559.0) was bricked by an older
build of the tool, before the 18-byte sense buffer was added to the pass-through request:

```
[trace] SCSI CDB 55 00 00 .. 00 3A to drive 58 bytes = 0x00000000   <- unlock (BB/30 + 16x FF)
SATA: command timed out.
SATA: reset timed out.
[trace] SCSI CDB 55 00 00 .. 00 3A to drive 58 bytes = 0xC000016B   <- set key (BB/30 + key)
```

The unlock succeeded, the set-key write timed out, and the drive was left unusable. The current
build has paired a Lite-On drive successfully but hasn't been tried on a Hitachi.

## What the binaries show

- **The recovery (11775.3) has no Hitachi path.** Its only drive-model strings are two DG-16D4S
  entries. The old-console writer (`sub_920894C8`, used on a Falcon) opens the drive, sends unlock
  and set-key back-to-back through `sub_92089430`, and closes it: no delay, readiness check or
  retry, for any drive.
- **The kernel waits only briefly per command.** Before every ATAPI request,
  `SataCdRomSelectDeviceAndSpinWhileBusy` spins up to 100 ms (1000 × 100 µs) for the busy bit to
  clear. This applies to the tool's commands too.
- **The kernel has a longer ready wait, but only at boot.** `SataCdRomInitialize` (11775) and
  `SataCdRomInitializeContinue` (21256) issue IOCTL `0x24800` up to 80 times, 250 ms apart
  (about 20 s), while it returns `0xC00000A3` (device not ready) or `0xC0000185` (I/O device
  error). It's skipped when `XboxHardwareInfo & 0x2000` is set.
- **The 21256 kernel's Hitachi-specific code is for XGD3 game discs, not pairing.** An "HCDF
  runtime patch" table matches all six kernel-listed GDR3120L revisions (`0046`/`0047`,
  `0058`/`0059`, `0078`/`0079`) by full INQUIRY string. It's activated only when authenticating an
  XGD3 disc (`SataCdRomXGD2AuthenticationSequence` → `SataCdRomActivateHCDFRuntimePatch`), using
  Hitachi vendor commands (`E7` read/write, enable flash exe, jump to RAM).

## Potential fix (not implemented)

A similar hang was seen with libxenon, where an extra wait for the drive to be ready before
sending a command avoided it. Neither the recovery nor the kernel waits between unlock and set-key,
so this would be the tool's own addition:

1. **Ready wait before each key write.** Before each MODE SELECT (unlock, set key, and A5/10 on
   the NEW path), issue IOCTL `0x24800` on the open `\Device\CdRom0` handle until it returns
   something other than `0xC00000A3`/`0xC0000185`, with the kernel's limits: up to 80 tries,
   250 ms apart. Give up without sending the write if the drive never becomes ready.
2. **Optional settle delay after the unlock**, if the ready wait alone isn't enough. Its length
   would need to come from testing.
3. **Trace both** in debug builds: each `0x24800` status and how long the wait took.

Open question: what exactly did the libxenon fix wait on (busy bit, ready bit, a fixed delay after
reset)? Matching it would make this more than a guess.

## Untested guess about the bricked drive

If an XGD3 disc had been authenticated in that drive during the same session, the drive may have
been running the kernel's HCDF patch code ("jump to RAM") when the key write arrived. Testing with
no disc in the drive would rule this out.

## Test plan

Use Hitachi drives that can be lost, with a debug build so the trace is on:

1. **No disc in the drive**, console fully powered off and on before each attempt.
2. **Current build first, unchanged** (it has the sense buffer), to see whether that alone avoids
   the hang. Record every `SCSI CDB` and `sense` line.
3. **If it still times out**, add the ready wait above and retry on another drive.
4. **Confirm pairing** by rebooting and reading a disc, since GDR3120L firmware rejects the key
   read (MODE SENSE page `0x25`).
5. Note the revision of every drive tested, and whether it survived.
