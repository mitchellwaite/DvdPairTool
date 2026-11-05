//------------------------------------------------------------------------------
// DvdPair.h  -  Pair an Xbox 360 DVD drive (ODD) to the console from the dashboard.
//
// Reverse-engineered from the 11775.3 Xenon recovery (default.xex). See
// PAIRING_ANALYSIS.md for the full analysis.
//
// The console's KeyVault is only read (XeKeysGetKey); the key is written to the drive
// alone. XeKeys* are exported by BOTH retail and devkit kernels (by ordinal), and the
// kernel side is only a thin marshaller to the hypervisor, so enforcement lives in the
// HYPERVISOR, driven by the XEX's signed system flags. So this runs on:
//   - a devkit / test kit, or
//   - a retail console whose HV privilege checks are patched out (RGH/JTAG).
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

// Returned by the AUTO pairing path for a DG-16D4S, DG-16D5S or DL10N, which can have
// different controller chips running D2S firmware. Nothing is written: the caller should
// ask the user and re-call with DVDP_ODD_OLD or DVDP_ODD_NEW.
#define DVDP_STATUS_CHOOSE_METHOD     ((DVDP_STATUS)0xE0000002L)

// Which ODD key-set sequence to use.
typedef enum {
    DVDP_ODD_AUTO = 0,  // old path, or defer to the caller on unknown fw / D4S / D5S / DL10N
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
    DVD_FAMILY_HITACHI,         // vendor "HL-DT-ST", product "DVD-ROM GDR3120L", 12 known fw revs
    DVD_FAMILY_BENQ,            // "PBDS    "/"VAD6038-64930C"; fw-6243 units report "SATA"/"DVD-ROM"; fw 6243/6493/0442/0443
    DVD_FAMILY_LITEON_D2S,      // vendor "PLDS    ", product "DG-16D2S", fw 7485/8385/9345/0251
    DVD_FAMILY_LITEON_D4S,      // vendor "PLDS    ", product "DG-16D4S", fw 9504/0225/0272/0401/1071/J504
    DVD_FAMILY_LITEON_D5S,      // product "DG-16D5S", fw 1175/1532; not in the kernel tables
    DVD_FAMILY_HITACHI_DL10N    // vendor "HL-DT-ST", product "DVD-ROM DL10N" (slim), fw 0500/0502
} DVD_DRIVE_FAMILY;

typedef struct _DVD_DRIVE_INFO {
    char             Vendor[9];     // INQUIRY bytes 8..15  + NUL
    char             Product[17];   // INQUIRY bytes 16..31 + NUL
    char             Revision[5];   // INQUIRY bytes 32..35 + NUL
    char             Inquiry28[29]; // the raw 28-byte field the recovery matches + NUL
    DVD_DRIVE_FAMILY Family;
    int              AskMethod;     // 1 if the model can use either pairing method (D4S/D5S/DL10N)
    int              KnownFirmware; // 1 if Revision is a known-good rev for Family (else unknown)
} DVD_DRIVE_INFO;

// Send SCSI INQUIRY (0x12) to \Device\CdRom0 and classify the drive.
DVDP_STATUS Odd_DetectDrive(DVD_DRIVE_INFO* outInfo);

// --- High level -------------------------------------------------------------

// Detect the connected drive and write the console's KeyVault DVD key into it, using the
// given method. The KeyVault is only read, never changed.
// DVDP_ODD_AUTO uses the old path, but returns without writing anything so the caller can
// re-call with DVDP_ODD_OLD or DVDP_ODD_NEW in these cases:
//   - DVDP_STATUS_CHOOSE_METHOD: DG-16D4S, DG-16D5S or DL10N, on any console.
//   - DVDP_STATUS_UNKNOWN_FIRMWARE: unrecognized firmware revision.
// DVDP_ODD_OLD or DVDP_ODD_NEW force that sequence regardless of what was detected.
DVDP_STATUS DvdPair_PairConnectedDrive(DVDP_ODD_METHOD method);

// Write the DVD key to the drive using a specific sequence (no detection, no KeyVault).
DVDP_STATUS Odd_SetDriveKeyMethod(const unsigned char key[DVDP_DVD_KEY_SIZE],
                                  DVDP_ODD_METHOD method);

// --- Building blocks (exposed for testing) ---------------------------------

// Read the DVD key currently stored in the console KeyVault.
DVDP_STATUS Dvd_GetConsoleKey(unsigned char outKey[DVDP_DVD_KEY_SIZE]);

// Drive-side: read the drive's current key / identity (MODE SENSE page 0x25).
// Not every drive supports this: some reject it with 0xC0000010 (sense 5/24h, invalid
// field in CDB). That says nothing about whether the drive can be paired.
DVDP_STATUS Odd_ReadDriveKey(unsigned char outKey[DVDP_DVD_KEY_SIZE]);

#ifdef __cplusplus
}
#endif
#endif // DVDPAIR_H
