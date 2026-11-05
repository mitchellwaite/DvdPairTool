//------------------------------------------------------------------------------
// DvdPair.h  -  Pair an Xbox 360 DVD drive (ODD) to the console from the dashboard.
//
// Reverse-engineered from the 11775.3 Xenon recovery (default.xex). See
// PAIRING_ANALYSIS.md for the full analysis.
//
// Needs KeyVault-write privilege. XeKeys* are exported by BOTH retail and devkit
// kernels (by ordinal); the kernel side is only a thin marshaller to the hypervisor
// (XeKeysSetKey -> HvxKeysMarshal op 0x36, XeKeysSaveKeyVault -> op 0x35) with no
// kernel-level privilege gate. Enforcement lives in the HYPERVISOR, driven by the
// XEX's signed system flags (the recovery is signed "Manufacturing Utility/Support
// Tool"). So this runs on:
//   - a devkit / test kit (your Falcon TK), or
//   - a retail console whose HV privilege checks are patched out (RGH/JTAG).
// On a stock retail console it is refused by the HV - not an artificial toggle.
//
// This performs drive<->console authentication pairing (interop/repair). It does
// not touch disc copy protection or code signing.
//------------------------------------------------------------------------------
#ifndef DVDPAIR_H
#define DVDPAIR_H

#ifdef __cplusplus
extern "C" {
#endif

// NTSTATUS-style return. >= 0 success, < 0 failure (e.g. 0xC00000xx).
typedef long DVDP_STATUS;

// Returned by the AUTO pairing path when the drive's firmware revision is not in the
// known list: the caller should ask the user whether to try the OLD or NEW method and
// re-call with that method explicitly. (Customer-defined error code; DVDP_SUCCESS is false.)
#define DVDP_STATUS_UNKNOWN_FIRMWARE  ((DVDP_STATUS)0xE0000001L)

// Which ODD key-set sequence to use.
typedef enum {
    DVDP_ODD_AUTO = 0,  // mirror the recovery: route by console + DG-16D4S; defer if fw unknown
    DVDP_ODD_OLD,       // old path: BB/30 unlock(FF) + BB/30 set  (recovery sub_920894C8)
    DVDP_ODD_NEW        // new path: BB/30 unlock + BB/30 set + A5/10 authenticate (sub_92089088)
} DVDP_ODD_METHOD;

#define DVDP_DVD_KEY_SIZE   16

// KeyVault key index of the DVD authentication key (confirmed from recovery).
#define KV_INDEX_DVD_KEY    0x1A

// ODD command transport (confirmed from recovery).
#define ODD_DEVICE_NAME     "\\Device\\CdRom0"
#define IOCTL_ODD_RAW_CMD   0x0004D014

// --- Drive identification ---------------------------------------------------

typedef enum {
    DVD_FAMILY_UNKNOWN = 0,
    // Identifiers verified against this build's kernel SataCdRom driver unless noted.
    DVD_FAMILY_SAMSUNG,         // vendor "TSSTcorp", product "DVD-ROM TS-H943A", fw ms25/ms28
    DVD_FAMILY_HITACHI,         // vendor "HL-DT-ST", product "DVD-ROM GDR3120L", fw 0059/0061
    DVD_FAMILY_BENQ,            // "PBDS    "/"VAD6038-64930C"; fw-6243 units report "SATA"/"DVD-ROM"
    DVD_FAMILY_LITEON_D2S,      // vendor "PLDS    ", product "DG-16D2S", fw 7485/8385/9345
    DVD_FAMILY_LITEON_D4S,      // vendor "PLDS    ", product "DG-16D4S", fw 9504 / J504(recovery)
    DVD_FAMILY_LITEON_D5S       // DG-16D5S: flash not writable, key can't be set - unsupported
} DVD_DRIVE_FAMILY;

typedef struct _DVD_DRIVE_INFO {
    char             Vendor[9];     // INQUIRY bytes 8..15  + NUL
    char             Product[17];   // INQUIRY bytes 16..31 + NUL
    char             Revision[5];   // INQUIRY bytes 32..35 + NUL
    char             Inquiry28[29]; // the raw 28-byte field the recovery matches + NUL
    DVD_DRIVE_FAMILY Family;
    int              IsDG16D4S;     // 1 if it matches the recovery's DG-16D4S table
    int              KnownFirmware; // 1 if Revision is a known-good rev for Family (else unknown)
} DVD_DRIVE_INFO;

// Send SCSI INQUIRY (0x12) to \Device\CdRom0 and classify the drive.
DVDP_STATUS Odd_DetectDrive(DVD_DRIVE_INFO* outInfo);

// Read XboxHardwareInfo.Flags; returns nonzero if this is a "new-generation" board
// (the recovery's (Flags & 0xF0000000) >= 0x40000000 test). Your Falcon returns 0.
int Dvd_IsNewGenerationConsole(void);

// --- High level -------------------------------------------------------------

// Detect the connected drive and pair it the way the recovery would on THIS console:
//   - older board (e.g. Falcon): plain BB/30 unlock+set with `key`, for every drive.
//   - newer board + DG-16D4S:    the recovery's D4S provisioning flow (see notes).
// If `key` is NULL, the console's current KeyVault DVD key is used (recommended for
// swapping test drives); pass alsoWriteKeyVault to also store `key` in the KeyVault.
// On an UNRECOGNIZED firmware revision this returns DVDP_STATUS_UNKNOWN_FIRMWARE without
// touching anything - call DvdPair_PairConnectedDriveEx with DVDP_ODD_OLD or _NEW.
DVDP_STATUS DvdPair_PairConnectedDrive(const unsigned char* key /*16 bytes or NULL*/,
                                       int alsoWriteKeyVault);

// Same, but choose the ODD key-set method explicitly. Pass DVDP_ODD_AUTO to mirror the
// recovery (and get DVDP_STATUS_UNKNOWN_FIRMWARE on an unknown rev); pass DVDP_ODD_OLD or
// DVDP_ODD_NEW to force that sequence regardless of what was detected.
DVDP_STATUS DvdPair_PairConnectedDriveEx(const unsigned char* key /*16 bytes or NULL*/,
                                         int alsoWriteKeyVault, DVDP_ODD_METHOD method);

// Write the DVD key to the drive using a specific sequence (no detection, no KeyVault).
DVDP_STATUS Odd_SetDriveKeyMethod(const unsigned char key[DVDP_DVD_KEY_SIZE],
                                  DVDP_ODD_METHOD method);

// --- High level -------------------------------------------------------------

// Read the console's current DVD key out of the KeyVault, then write it into the
// currently-connected drive. Use this after swapping in a different test drive so
// the drive matches THIS console. Does not modify the KeyVault, so nothing to back up.
DVDP_STATUS DvdPair_AdoptConsoleKeyIntoDrive(void);

// Full re-pair exactly like the recovery: make a fresh random 16-byte key, store it
// in the KeyVault (index 0x1A) + save the KeyVault, and write it into the drive.
// WARNING: persists a new key to NAND. Back up your KeyVault first.
DVDP_STATUS DvdPair_RepairWithNewKey(void);

// Pair using a caller-supplied 16-byte key: optionally write to KeyVault (+save),
// always write to the drive.
DVDP_STATUS DvdPair_PairWithKey(const unsigned char key[DVDP_DVD_KEY_SIZE],
                                int alsoWriteKeyVault);

// --- Building blocks (exposed for testing) ---------------------------------

// Read the DVD key currently stored in the console KeyVault.
DVDP_STATUS Dvd_GetConsoleKey(unsigned char outKey[DVDP_DVD_KEY_SIZE]);

// Store a DVD key in the console KeyVault (index 0x1A) and persist it.
DVDP_STATUS Dvd_SetConsoleKey(const unsigned char key[DVDP_DVD_KEY_SIZE]);

// Drive-side: unlock the drive's key slot, then write the 16-byte key into it.
DVDP_STATUS Odd_SetDriveKey(const unsigned char key[DVDP_DVD_KEY_SIZE]);

// Drive-side: read the drive's current key / identity (vendor cmd 0x25).
DVDP_STATUS Odd_ReadDriveKey(unsigned char outKey[DVDP_DVD_KEY_SIZE]);

#ifdef __cplusplus
}
#endif
#endif // DVDPAIR_H
