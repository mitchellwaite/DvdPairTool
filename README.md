# DVD Drive Pairing Tool

A text-mode Xbox 360 app that pairs a DVD drive to the console, so drives can be swapped between consoles without reinstalling the OS or reflashing. 

> [!WARNING]
> This has been tested on a console with MTK based drives (Samsung, BenQ, LiteOn). GDR-3120L
> hasn't been verified and these drives hard brick easily.

**This only works on consoles with a patched hypervisor that allows access to the DVD key**. A stock hypervisor refuses to hand out the key (`0xC8000002`).

This tool does not change the DVD key, it will only read it from the Keyvault and push it to the DVD drive.

## Controls

- **A** — pair drive to this console (writes the console's current KeyVault DVD key into the
  drive; KeyVault untouched). On a DG-16D4S, DG-16D5S, DL10N or unrecognized firmware it asks
  X=OLD / Y=NEW. Once an attempt has run, the tool only offers "Press A to reboot".
- **X** — read and print the drive's current key. Not every drive supports this: `0xC0000010`
  means the drive rejected the request, not that it can't be paired.
- **BACK** — return to the dashboard (no reboot).

## Supported drives

| Manufacturer | Model | Firmware revisions | Pairing path |
|---|---|---|---|
| Hitachi-LG | GDR3120L | `0027` `0032` `0036` `0037` `0040` `0046` `0047` `0058` `0059` `0061` `0078` `0079` | OLD (not verified, see warning) |
| Hitachi-LG | DL10N | `0500` `0502` | Asks: OLD or NEW |
| Samsung | TS-H943A | `ms25` `ms28` | OLD |
| BenQ | VAD6038 | `6243` `6493` `0442` `0443` | OLD (every revision) |
| Lite-On | DG-16D2S | `7485` `8385` `9345` `0251` | OLD |
| Lite-On | DG-16D4S | `9504` `0225` `0272` `0401` `1071` `J504` | Asks: OLD or NEW |
| Lite-On | DG-16D5S | `1175` `1532` | Asks: OLD or NEW |

- **OLD** is `BB/30` unlock + set key. **NEW** adds an `A5/10` authenticate with the same key.
- A revision that isn't listed is reported as `UNRECOGNIZED`, and the tool asks for OLD or NEW
  instead of guessing.

## TODO

RGLoader may be unsupported since it is missing the patch to de-restrict HvxKeysGetKey. A future update of JRunner will add this patch. A future version of this tool may add a manual HV patch step at startup for 17489 dev kernel.

## Notes

- This reproduces the pairing step of the 11775.3 recovery; see [PAIRING_ANALYSIS.md](PAIRING_ANALYSIS.md) for how that works.
- See [HITACHI.md](HITACHI.md) for notes on the GDR-3120L.

