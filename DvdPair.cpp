//------------------------------------------------------------------------------
// DvdPair.cpp  -  Pair an Xbox 360 DVD drive (ODD) to the console.
//
// Faithful re-implementation of the pairing logic in the 11775.3 Xenon recovery
// (default.xex). See PAIRING_ANALYSIS.md.
//
// Build as part of a DEVKIT / TEST KIT title (XDK). The native calls below are the
// ones the recovery itself uses. Offsets/opcodes marked "confirmed" came straight
// out of the recovery; the two bytes marked "observed constant" are reserved fields
// that were always the same value in the recovery and are reproduced verbatim.
//------------------------------------------------------------------------------
#include "DvdPair.h"
#include <string.h>

//==============================================================================
// Native kernel / NT imports.
//
// On the real XDK build (_XBOX) we pull the NT types and APIs from <xtl.h> and only
// declare the handful of kernel exports that have no public header (XeKeys*). When
// built elsewhere (reference/portability), we declare compatible local prototypes so
// the file still parses. Either way the symbols resolve from xboxkrnl.lib.
//==============================================================================
// xtl.h does not declare the NT native APIs or ANSI_STRING/OBJECT_ATTRIBUTES, so we
// declare them ourselves (C linkage -> they resolve against xboxkrnl.lib by name).
// The struct layouts match the kernel's, which is all that matters at the call site.
extern "C" {
typedef struct _DVDP_STRING {        // ANSI_STRING
    unsigned short Length;
    unsigned short MaximumLength;
    char*          Buffer;
} DVDP_STRING;
typedef struct _DVDP_OBJECT_ATTRIBUTES {   // OBJECT_ATTRIBUTES (Xbox layout)
    void*          RootDirectory;
    DVDP_STRING*   ObjectName;
    unsigned long  Attributes;
} DVDP_OBJECT_ATTRIBUTES;
typedef struct _DVDP_IO_STATUS_BLOCK {
    long           Status;
    unsigned long  Information;
} DVDP_IO_STATUS_BLOCK;
long NtOpenFile(void** FileHandle, unsigned long DesiredAccess,
                DVDP_OBJECT_ATTRIBUTES* ObjectAttributes, DVDP_IO_STATUS_BLOCK* IoStatusBlock,
                unsigned long ShareAccess, unsigned long OpenOptions);
long NtClose(void* Handle);
long NtDeviceIoControlFile(void* FileHandle, void* Event, void* ApcRoutine, void* ApcContext,
                           DVDP_IO_STATUS_BLOCK* IoStatusBlock, unsigned long IoControlCode,
                           void* InputBuffer, unsigned long InputBufferLength,
                           void* OutputBuffer, unsigned long OutputBufferLength);
long NtWaitForSingleObjectEx(void* Handle, char WaitMode, int Alertable, void* Timeout);
// KeyVault exports (no public header; confirmed from xboxkrnld.exe.i64).
long XeKeysSetKey(unsigned long Index, void* Buffer, unsigned long Length);
long XeKeysGetKey(unsigned long Index, void* Buffer, unsigned long* Length);
long XeKeysSaveKeyVault(void);
long XeKeysGenerateRandomKey(void* Buffer, unsigned long Length);
// Kernel-exported hardware descriptor (ordinal 0x156); first DWORD is Flags.
struct DVDP_XBOX_HARDWARE_INFO { unsigned long Flags; };
extern DVDP_XBOX_HARDWARE_INFO XboxHardwareInfo;
}

// Recovery's known DG-16D4S provisioning constants (from byte_92017040).
// These are FIXED Microsoft test values, not a per-console key.
static const unsigned char kD4S_BlankMarker[16] =
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const unsigned char kD4S_KeyConstA[16] =     // byte_92017040 + 0x20
    { 0xFA,0xEE,0xDD,0xCC,0xBB,0xAA,0x99,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x00 };
static const unsigned char kD4S_AuthConstB[16] =    // byte_92017040 + 0x30
    { 0x64,0x7F,0x45,0xE8,0x6B,0x7C,0x2B,0xE9,0x36,0xCB,0x0F,0x0B,0x92,0x64,0x45,0xB5 };

#define DVDP_SUCCESS(s)  ((DVDP_STATUS)(s) >= 0)
#define DVDP_PENDING     (0x00000103L)         // STATUS_PENDING

//==============================================================================
// ODD command block + raw IOCTL request (exact layout from the recovery).
//==============================================================================

// Command block: [u16 size][6 reserved][opcode][modifier][payload...]
// Max payload we need is 16 bytes of key; recovery zero-pads to 56.
#pragma pack(push, 1)
typedef struct _ODD_COMMAND {
    unsigned short Size;             // +0x00 total size of this block
    unsigned char  Reserved0[6];     // +0x02 (zero)
    unsigned char  Opcode;           // +0x08 e.g. 0xBB / 0xA5 / 0x25
    unsigned char  Modifier;         // +0x09 e.g. 0x30 / 0x10
    unsigned char  Payload[46];      // +0x0A key / data (recovery used up to 56 total)
} ODD_COMMAND;

// 36-byte request handed to IOCTL 0x4D014; it points at the ODD_COMMAND block.
typedef struct _ODD_RAW_REQUEST {
    unsigned short RequestSize;      // +0x00 = 0x0024 (36)
    unsigned short Reserved0;        // +0x02 = 0
    unsigned char  Reserved1[3];     // +0x04 = 0
    unsigned char  Direction;        // +0x07 0 = to drive, 1 = from drive
    unsigned long  TransferLength;   // +0x08 = Command.Size + 2
    unsigned long  Reserved2;        // +0x0C = 0
    ODD_COMMAND*   Command;          // +0x10 pointer to the command block
    unsigned char  Signature;        // +0x14 = 0x55 ('U')   (observed constant)
    unsigned char  Reserved3[6];     // +0x15 = 0
    unsigned short TransferLength2;  // +0x1B = Command.Size + 2
    unsigned char  Reserved4[3];     // +0x1D = 0
    unsigned long  Reserved5;        // +0x20 = 0  (pads struct to 0x24)
} ODD_RAW_REQUEST;
#pragma pack(pop)

// The recovery hands a 36-byte (0x24) request to IOCTL 0x4D014; keep it exact.
typedef char ODD_RAW_REQUEST_size_check[(sizeof(ODD_RAW_REQUEST) == 0x24) ? 1 : -1];

// --- Open / close \Device\CdRom0 exactly as the recovery does ----------------
static DVDP_STATUS OddOpen(void** outHandle)
{
    DVDP_STRING name;
    name.Buffer        = (char*)ODD_DEVICE_NAME;
    name.Length        = (unsigned short)strlen(ODD_DEVICE_NAME);
    name.MaximumLength = 64;

    DVDP_OBJECT_ATTRIBUTES oa;
    oa.RootDirectory = 0;
    oa.ObjectName    = &name;
    oa.Attributes    = 64;           // OBJ_CASE_INSENSITIVE-ish; value from recovery

    DVDP_IO_STATUS_BLOCK iosb;
    *outHandle = 0;
    // DesiredAccess 0x80100000, ShareAccess 1, OpenOptions 0x20 -> from recovery.
    return NtOpenFile(outHandle, 0x80100000, &oa, &iosb, 1, 0x20);
}

// --- Synchronous NtDeviceIoControlFile wrapper (recovery sub_92027480) -------
static DVDP_STATUS OddIoctl(void* handle, ODD_RAW_REQUEST* req)
{
    DVDP_IO_STATUS_BLOCK iosb;
    iosb.Status = DVDP_PENDING;

    long st = NtDeviceIoControlFile(handle, 0, 0, 0, &iosb, IOCTL_ODD_RAW_CMD,
                                    req, sizeof(ODD_RAW_REQUEST), 0, 0);
    if (st == DVDP_PENDING) {
        st = NtWaitForSingleObjectEx(handle, 1 /*KernelMode*/, 0, 0);
        if (DVDP_SUCCESS(st))
            st = iosb.Status;
    }
    return st;
}

// --- Issue one vendor command to the drive -----------------------------------
// opcode/modifier/payload -> command block; direction 0 = write to drive.
static DVDP_STATUS OddSendCommand(void* handle,
                                  unsigned char opcode, unsigned char modifier,
                                  unsigned char direction,
                                  const unsigned char* payload, int payloadLen,
                                  int blockTotalSize)
{
    ODD_COMMAND cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Size     = (unsigned short)blockTotalSize;   // 56 for BB/30, 24 for A5/10
    cmd.Opcode   = opcode;
    cmd.Modifier = modifier;
    if (payload && payloadLen > 0)
        memcpy(cmd.Payload, payload, payloadLen);

    ODD_RAW_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.RequestSize     = 0x0024;
    req.Direction       = direction;
    req.TransferLength  = (unsigned long)(blockTotalSize + 2);
    req.Command         = &cmd;
    req.Signature       = 0x55;
    req.TransferLength2 = (unsigned short)(blockTotalSize + 2);

    return OddIoctl(handle, &req);
}

//==============================================================================
// Drive-side public helpers.
//==============================================================================

static DVDP_STATUS Odd_AuthenticateKey(void* h, const unsigned char key[DVDP_DVD_KEY_SIZE]);

DVDP_STATUS Odd_SetDriveKeyMethod(const unsigned char key[DVDP_DVD_KEY_SIZE],
                                  DVDP_ODD_METHOD method)
{
    void* h = 0;
    DVDP_STATUS st = OddOpen(&h);
    if (!DVDP_SUCCESS(st))
        return st;                                   // "Didn't find a DVD Drive"

    // Both methods: unlock (BB/30 + 16x0xFF) then set (BB/30 + key), block size 56.
    unsigned char ff[DVDP_DVD_KEY_SIZE];
    memset(ff, 0xFF, sizeof(ff));
    st = OddSendCommand(h, 0xBB, 0x30, 0, ff, DVDP_DVD_KEY_SIZE, 56);
    if (DVDP_SUCCESS(st))
        st = OddSendCommand(h, 0xBB, 0x30, 0, key, DVDP_DVD_KEY_SIZE, 56);

    // NEW method additionally authenticates with the key (A5/10, block size 24),
    // matching the extra handshake the recovery's new-console path does for D4S.
    if (DVDP_SUCCESS(st) && method == DVDP_ODD_NEW)
        st = Odd_AuthenticateKey(h, key);

    NtClose(h);
    return st;                                        // caller logs "Failed to ... ODD"
}

DVDP_STATUS Odd_SetDriveKey(const unsigned char key[DVDP_DVD_KEY_SIZE])
{
    return Odd_SetDriveKeyMethod(key, DVDP_ODD_OLD); // old path: BB/30 unlock+set
}

DVDP_STATUS Odd_ReadDriveKey(unsigned char outKey[DVDP_DVD_KEY_SIZE])
{
    void* h = 0;
    DVDP_STATUS st = OddOpen(&h);
    if (!DVDP_SUCCESS(st))
        return st;

    // vendor cmd 0x25 (recovery sub_920D7EB8), reads 16 bytes back, block size 24.
    ODD_COMMAND cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Size   = 24;
    cmd.Opcode = 0x25;

    ODD_RAW_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.RequestSize     = 0x0024;
    req.Direction       = 1;                          // from drive
    req.TransferLength  = 24 + 2;
    req.Command         = &cmd;
    req.Signature       = 0x55;
    req.TransferLength2 = 24 + 2;

    st = OddIoctl(h, &req);
    if (DVDP_SUCCESS(st))
        memcpy(outKey, cmd.Payload, DVDP_DVD_KEY_SIZE);
    NtClose(h);
    return st;
}

// A5/10 authenticate with a 16-byte key (recovery sub_920D7DD8, block size 24).
static DVDP_STATUS Odd_AuthenticateKey(void* h, const unsigned char key[DVDP_DVD_KEY_SIZE])
{
    return OddSendCommand(h, 0xA5, 0x10, 0, key, DVDP_DVD_KEY_SIZE, 24);
}

//==============================================================================
// Drive identification (SCSI INQUIRY 0x12) and console generation.
//==============================================================================

static DVD_DRIVE_FAMILY ClassifyDrive(const DVD_DRIVE_INFO* di)
{
    // INQUIRY vendor (bytes 8..15, fixed 8, space-padded) and product (16..31, 16).
    // These are the exact strings from the kernel's SataCdRom drive-match table. The
    // base set is in this build's kernel (xboxkrnld 4C6787B8, table @0x80044C28 and the
    // SataCdRomVendorPLDS / SataCdRomRevisionPLDS7485/8385 symbols). The COMPLETE set
    // below (BenQ + extra firmware revisions) is verified against the newer SDK kernel
    // ../21256.18_Xenon_Recovery_with_Symbols (xboxkrnlc 55EFF578), whose match table
    // holds these literal 28-char records (vendor8+product16+rev4):
    //   "HL-DT-ST" "DVD-ROM GDR3120L" rev 0046/0047/0058/0059/0078/0079   (Hitachi-LG)
    //   "TSSTcorp" "DVD-ROM TS-H943A" rev ms25/ms28                       (Samsung)
    //   "PBDS    " "VAD6038-64930C"   (no rev)                            (BenQ)
    //   "PLDS    " "DG-16D2S"         rev 7485/8385/9345                  (Lite-On DG-16D2S)
    //   "PLDS    " "DG-16D4S"         rev 9504  (+ "J504" from 11775.3 recovery table)
    //   "SATA    " "DVD-ROM"          rev 6243  (generic SATA fallback)
    // Note the vendors are distinct: BenQ = "PBDS", Lite-On = "PLDS". DG-16D5S is omitted
    // on purpose (its flash isn't writable, so the key can't be set).

    // BenQ (Philips/BenQ lineage): normally vendor "PBDS" product "VAD6038-64930C".
    // Confirmed on hardware: the firmware-"6243" units report generically as vendor
    // "SATA" product "DVD-ROM" instead (the kernel lists that as a distinct 28-char
    // record right after the VAD6038 one). Treat both as BenQ-class. Older units may
    // also read "PHILIPS"/"BENQ".
    if (!strncmp(di->Vendor, "PBDS", 4) || !strncmp(di->Vendor, "BENQ", 4) ||
        !strncmp(di->Vendor, "PHILIPS", 7) || strstr(di->Product, "VAD6038") ||
        (!strncmp(di->Vendor, "SATA", 4) && !strncmp(di->Revision, "6243", 4)) ||
        (!strncmp(di->Vendor, "SATA", 4) && strstr(di->Product, "DVD-ROM")))
        return DVD_FAMILY_BENQ;
    // Lite-On: vendor "PLDS" (older units "LITE-ON").
    if (!strncmp(di->Vendor, "PLDS", 4) || !strncmp(di->Vendor, "LITE-ON", 7)) {
        if (strstr(di->Product, "DG-16D5S")) return DVD_FAMILY_LITEON_D5S;  // detect only; flash not writable
        if (strstr(di->Product, "DG-16D4S")) return DVD_FAMILY_LITEON_D4S;
        if (strstr(di->Product, "DG-16D2S") ||
            !strncmp(di->Revision, "7485", 4) || !strncmp(di->Revision, "8385", 4) ||
            !strncmp(di->Revision, "9345", 4))
            return DVD_FAMILY_LITEON_D2S;
        return DVD_FAMILY_LITEON_D2S;        // default any Lite-On to the D2S-class path
    }
    // Samsung / Toshiba-Samsung.
    if (!strncmp(di->Vendor, "TSSTcorp", 8) || strstr(di->Product, "TS-H943"))
        return DVD_FAMILY_SAMSUNG;
    // Hitachi-LG (any GDR3120L firmware: 0046/0047/0058/0059/0078/0079...).
    if (!strncmp(di->Vendor, "HL-DT-ST", 8) || strstr(di->Product, "GDR3120L") ||
        strstr(di->Product, "GDR-3120L"))
        return DVD_FAMILY_HITACHI;
    return DVD_FAMILY_UNKNOWN;
}

// Known-good firmware revisions per family (union of the 11775.3 and 21256.18 kernels).
static int IsKnownFirmware(DVD_DRIVE_FAMILY fam, const char* rev, const char* product)
{
    const char* const* list = 0;
    static const char* hitachi[] = {"0046","0047","0058","0059","0061","0078","0079",0};
    static const char* samsung[] = {"ms25","ms28",0};
    static const char* d2s[]     = {"7485","8385","9345",0};
    static const char* d4s[]     = {"9504","J504",0};
    static const char* benq[]    = {"6243",0};  // + VAD6038 matched by product
    switch (fam) {
        case DVD_FAMILY_HITACHI:    list = hitachi; break;
        case DVD_FAMILY_SAMSUNG:    list = samsung; break;
        case DVD_FAMILY_LITEON_D2S: list = d2s;     break;
        case DVD_FAMILY_LITEON_D4S: list = d4s;     break;
        case DVD_FAMILY_BENQ:
            if (strstr(product, "VAD6038")) return 1;
            list = benq; break;
        default: return 0;          // UNKNOWN family or D5S -> firmware not "known-good"
    }
    for (int i = 0; list[i]; ++i)
        if (!strncmp(rev, list[i], 4)) return 1;
    return 0;
}

DVDP_STATUS Odd_DetectDrive(DVD_DRIVE_INFO* outInfo)
{
    memset(outInfo, 0, sizeof(*outInfo));

    void* h = 0;
    DVDP_STATUS st = OddOpen(&h);
    if (!DVDP_SUCCESS(st))
        return st;

    // Standard SCSI INQUIRY: opcode 0x12, allocation length 36, 36 bytes back.
    // NOTE: the raw passthrough layout for read commands should be confirmed against
    // sub_920D7AC8 / sub_920D7CA8; the response's vendor/product/revision sit at
    // INQUIRY response offsets 8..35 (the exact 28-byte field the recovery matches).
    ODD_COMMAND cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.Size     = 36;
    cmd.Opcode   = 0x12;              // INQUIRY
    cmd.Modifier = 0x00;
    cmd.Payload[4] = 36;             // allocation length (EVPD=0)

    ODD_RAW_REQUEST req;
    memset(&req, 0, sizeof(req));
    req.RequestSize     = 0x0024;
    req.Direction       = 1;         // from drive
    req.TransferLength  = 36 + 2;
    req.Command         = &cmd;
    req.Signature       = 0x55;
    req.TransferLength2 = 36 + 2;

    st = OddIoctl(h, &req);
    NtClose(h);
    if (!DVDP_SUCCESS(st))
        return st;

    const unsigned char* resp = cmd.Payload;   // 36-byte INQUIRY response
    memcpy(outInfo->Vendor,   resp + 8,  8);  outInfo->Vendor[8]   = 0;
    memcpy(outInfo->Product,  resp + 16, 16); outInfo->Product[16] = 0;
    memcpy(outInfo->Revision, resp + 32, 4);  outInfo->Revision[4] = 0;
    memcpy(outInfo->Inquiry28, resp + 8, 28); outInfo->Inquiry28[28] = 0;

    outInfo->Family = ClassifyDrive(outInfo);
    // The recovery matches the exact 28-byte strings "PLDS    DG-16D4S        J504"
    // and "...9504"; treat any DG-16D4S as the special case.
    outInfo->IsDG16D4S = (outInfo->Family == DVD_FAMILY_LITEON_D4S);
    outInfo->KnownFirmware = IsKnownFirmware(outInfo->Family, outInfo->Revision,
                                             outInfo->Product);
    return st;
}

int Dvd_IsNewGenerationConsole(void)
{
    return ((XboxHardwareInfo.Flags & 0xF0000000u) >= 0x40000000u) ? 1 : 0;
}

//==============================================================================
// Console-side (KeyVault) public helpers.
//==============================================================================

DVDP_STATUS Dvd_GetConsoleKey(unsigned char outKey[DVDP_DVD_KEY_SIZE])
{
    unsigned long len = DVDP_DVD_KEY_SIZE;
    return XeKeysGetKey(KV_INDEX_DVD_KEY, outKey, &len);
}

DVDP_STATUS Dvd_SetConsoleKey(const unsigned char key[DVDP_DVD_KEY_SIZE])
{
    DVDP_STATUS st = XeKeysSetKey(KV_INDEX_DVD_KEY, (void*)key, DVDP_DVD_KEY_SIZE);
    if (!DVDP_SUCCESS(st))
        return st;                                   // "Failed to set DVD Key in KeyVault"
    return XeKeysSaveKeyVault();                      // persist to NAND
}

//==============================================================================
// High-level flows.
//==============================================================================

DVDP_STATUS DvdPair_AdoptConsoleKeyIntoDrive(void)
{
    unsigned char key[DVDP_DVD_KEY_SIZE];
    DVDP_STATUS st = Dvd_GetConsoleKey(key);
    if (!DVDP_SUCCESS(st))
        return st;
    return Odd_SetDriveKey(key);                      // console unchanged
}

DVDP_STATUS DvdPair_PairWithKey(const unsigned char key[DVDP_DVD_KEY_SIZE],
                                int alsoWriteKeyVault)
{
    if (alsoWriteKeyVault) {
        DVDP_STATUS st = Dvd_SetConsoleKey(key);
        if (!DVDP_SUCCESS(st))
            return st;
    }
    return Odd_SetDriveKey(key);
}

DVDP_STATUS DvdPair_RepairWithNewKey(void)
{
    // Recovery generates a random key, validates it, retries until valid. A simple
    // non-zero check is enough here; the drive/KeyVault reject obviously bad keys.
    unsigned char key[DVDP_DVD_KEY_SIZE];
    for (int tries = 0; tries < 8; ++tries) {
        DVDP_STATUS st = XeKeysGenerateRandomKey(key, DVDP_DVD_KEY_SIZE);
        if (!DVDP_SUCCESS(st))
            return st;
        int allZero = 1;
        for (int i = 0; i < DVDP_DVD_KEY_SIZE; ++i)
            if (key[i]) { allZero = 0; break; }
        if (!allZero)
            break;
    }
    return DvdPair_PairWithKey(key, /*alsoWriteKeyVault=*/1);
}

// Faithful replica of the recovery's DG-16D4S branch (sub_92089088, new-console only).
// Writes FIXED Microsoft test constants, NOT a per-console key. Provided for parity;
// for actually pairing a D4S to your own console you normally want the generic BB/30
// path with your real key instead.
static DVDP_STATUS Odd_ProvisionDG16D4S(void)
{
    void* h = 0;
    DVDP_STATUS st = OddOpen(&h);
    if (!DVDP_SUCCESS(st))
        return st;

    unsigned char cur[DVDP_DVD_KEY_SIZE];
    // read current drive key (vendor 0x25) into cur
    ODD_COMMAND rc; memset(&rc, 0, sizeof(rc)); rc.Size = 24; rc.Opcode = 0x25;
    ODD_RAW_REQUEST rr; memset(&rr, 0, sizeof(rr));
    rr.RequestSize = 0x0024; rr.Direction = 1; rr.TransferLength = 26;
    rr.Command = &rc; rr.Signature = 0x55; rr.TransferLength2 = 26;
    st = OddIoctl(h, &rr);
    if (!DVDP_SUCCESS(st)) { NtClose(h); return st; }
    memcpy(cur, rc.Payload, DVDP_DVD_KEY_SIZE);

    if (memcmp(cur, kD4S_BlankMarker, DVDP_DVD_KEY_SIZE) == 0) {
        // blank/factory drive: write CONST_A then authenticate with CONST_B
        unsigned char ff[DVDP_DVD_KEY_SIZE]; memset(ff, 0xFF, sizeof(ff));
        st = OddSendCommand(h, 0xBB, 0x30, 0, ff,            DVDP_DVD_KEY_SIZE, 56);
        if (DVDP_SUCCESS(st))
            st = OddSendCommand(h, 0xBB, 0x30, 0, kD4S_KeyConstA, DVDP_DVD_KEY_SIZE, 56);
        if (DVDP_SUCCESS(st))
            st = Odd_AuthenticateKey(h, kD4S_AuthConstB);
        NtClose(h);
        return st;
    }

    NtClose(h);
    // already-keyed drive: recovery only syncs the KeyVault to CONST_A
    return Dvd_SetConsoleKey(kD4S_KeyConstA);
}

DVDP_STATUS DvdPair_PairConnectedDriveEx(const unsigned char* key, int alsoWriteKeyVault,
                                         DVDP_ODD_METHOD method)
{
    DVD_DRIVE_INFO di;
    DVDP_STATUS st = Odd_DetectDrive(&di);
    if (!DVDP_SUCCESS(st))
        return st;                       // no drive / INQUIRY failed

    // DG-16D5S flash is not writable - the key cannot be set in the drive. Refuse
    // up front instead of failing obscurely at the BB/30 write.
    if (di.Family == DVD_FAMILY_LITEON_D5S)
        return (DVDP_STATUS)0xC00000A3L; // "can't pair this drive"

    if (method == DVDP_ODD_AUTO) {
        // Unrecognized firmware revision: don't guess. Report it so the caller can ask
        // the user to pick DVDP_ODD_OLD or DVDP_ODD_NEW and re-call. Nothing written yet.
        if (!di.KnownFirmware)
            return DVDP_STATUS_UNKNOWN_FIRMWARE;
        // Known firmware: mirror the recovery. A DG-16D4S on a new-generation board uses
        // the recovery's D4S provisioning flow (fixed test constants); everything else
        // (and every drive on an older board such as a Falcon) uses the old BB/30 path.
        if (di.IsDG16D4S && Dvd_IsNewGenerationConsole())
            return Odd_ProvisionDG16D4S();
        method = DVDP_ODD_OLD;
    }
    // (An explicit DVDP_ODD_OLD / _NEW overrides all of the above, including for a D4S.)

    unsigned char local[DVDP_DVD_KEY_SIZE];
    if (!key) {                          // default: use the console's existing key
        st = Dvd_GetConsoleKey(local);
        if (!DVDP_SUCCESS(st))
            return st;
        key = local;
    } else if (alsoWriteKeyVault) {
        st = Dvd_SetConsoleKey(key);
        if (!DVDP_SUCCESS(st))
            return st;
    }
    return Odd_SetDriveKeyMethod(key, method);
}

DVDP_STATUS DvdPair_PairConnectedDrive(const unsigned char* key, int alsoWriteKeyVault)
{
    return DvdPair_PairConnectedDriveEx(key, alsoWriteKeyVault, DVDP_ODD_AUTO);
}
