//------------------------------------------------------------------------------
// DvdPair.cpp  -  Pair an Xbox 360 DVD drive (ODD) to the console.
//
// Faithful re-implementation of the pairing logic in the 11775.3 Xenon recovery
// (default.xex). See PAIRING_ANALYSIS.md.
//
// Build as part of a DEVKIT / TEST KIT title (XDK). The native calls below are the
// ones the recovery itself uses. The drive is driven with SCSI commands through the
// kernel's pass-through IOCTL, laid out as in the recovery and the kernel's own callers.
//------------------------------------------------------------------------------
#include "DvdPair.h"
#include <string.h>

// Debug builds print each step and its status. Key bytes are never printed.
#ifdef DVDP_TRACE_ENABLED
#include "OutputConsole.h"
#define DVDP_TRACE(...) dprintf(__VA_ARGS__)
#else
#define DVDP_TRACE(...)
#endif

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
// KeyVault export (no public header; confirmed from xboxkrnld.exe.i64). This tool only
// ever reads the KeyVault.
long XeKeysGetKey(unsigned long Index, void* Buffer, unsigned long* Length);
#ifdef DVDP_TRACE_ENABLED
// Returns the hypervisor's live permission/length dword for a key (low 16 bits = length).
unsigned long XeKeysGetKeyProperties(unsigned long Index);
struct DVDP_XBOX_KRNL_VERSION { unsigned short Major, Minor, Build, Qfe; };
__declspec(dllimport) extern DVDP_XBOX_KRNL_VERSION XboxKrnlVersion;
#endif
}

// Written to the key page to unlock the drive's key slot (recovery sub_920D7B80).
static const unsigned char kUnlockKey[16] =
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

#define DVDP_SUCCESS(s)  ((DVDP_STATUS)(s) >= 0)
#define DVDP_PENDING     (0x00000103L)         // STATUS_PENDING

#define SCSI_INQUIRY         0x12
#define SCSI_MODE_SELECT10   0x55
#define SCSI_MODE_SENSE10    0x5A

// Mode pages the recovery uses. PAGE_SELECT_* values carry the PS bit (0x80).
#define ODD_PAGE_SELECT_SET_KEY       0xBB    // length 0x30
#define ODD_PAGE_SELECT_AUTHENTICATE  0xA5    // length 0x10
#define ODD_PAGE_SENSE_DRIVE_KEY      0x25

// ModeDataLength excludes itself, so the transfer is always this value + 2.
#define ODD_MODE_DATA_LENGTH_SET_KEY  56      // 8 header + 2 page + 48 data - 2
#define ODD_MODE_DATA_LENGTH_SHORT    24      // 8 header + 2 page + 16 data - 2

//==============================================================================
// SCSI pass-through request and mode parameter list. [kernel xboxkrnld 4C6787B8:
// SataCdRomStartScsiPassThrough, SataCdRomSendAP20Sense/Select; recovery sub_920D7A40,
// sub_920D7AC8, sub_920D7CA8]
//==============================================================================

// Mode parameter list: the data buffer of MODE SELECT / MODE SENSE. The u16 keeps it on
// an even address, which the kernel requires of pass-through buffers.
typedef struct _ODD_MODE_PAGE {
    unsigned short ModeDataLength;   // +0x00 big-endian, excludes this field
    unsigned char  Reserved[6];      // +0x02 rest of the mode header (zero)
    unsigned char  PageCode;         // +0x08
    unsigned char  PageLength;       // +0x09
    unsigned char  Data[48];         // +0x0A key at the start, zero-padded
} ODD_MODE_PAGE;
typedef char ODD_MODE_PAGE_size_check[(sizeof(ODD_MODE_PAGE) == 58) ? 1 : -1];

// 36-byte request handed to IOCTL 0x4D014.
typedef struct _ODD_SCSI_REQUEST {
    unsigned short Length;           // +0x00 = 0x0024 (36)
    unsigned char  Reserved0[5];     // +0x02 = 0
    unsigned char  DataIn;           // +0x07 0 = to drive, 1 = from drive
    unsigned long  DataLength;       // +0x08 even, at most 0x20000
    unsigned long  Reserved1;        // +0x0C = 0
    void*          DataBuffer;       // +0x10 even address
    unsigned char  Cdb[16];          // +0x14
} ODD_SCSI_REQUEST;
typedef char ODD_SCSI_REQUEST_size_check[(sizeof(ODD_SCSI_REQUEST) == 0x24) ? 1 : -1];

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
    DVDP_STATUS st = NtOpenFile(outHandle, 0x80100000, &oa, &iosb, 1, 0x20);
    DVDP_TRACE("  [trace] NtOpenFile(%s) = 0x%08X\n", ODD_DEVICE_NAME, (unsigned)st);
    return st;
}

// --- Synchronous NtDeviceIoControlFile wrapper (recovery sub_92027480) -------
static DVDP_STATUS OddIoctl(void* handle, ODD_SCSI_REQUEST* req)
{
    DVDP_IO_STATUS_BLOCK iosb;
    iosb.Status = DVDP_PENDING;

    // 18-byte sense buffer as the output, like the recovery's old-console sender
    // (sub_92089430). The kernel accepts an output length of 0 or at least 0x12.
    __declspec(align(4)) unsigned char sense[18];
    memset(sense, 0, sizeof(sense));

    long st = NtDeviceIoControlFile(handle, 0, 0, 0, &iosb, IOCTL_ODD_RAW_CMD,
                                    req, sizeof(ODD_SCSI_REQUEST), sense, sizeof(sense));
    if (st == DVDP_PENDING) {
        st = NtWaitForSingleObjectEx(handle, 1 /*UserMode*/, 0, 0);
        if (DVDP_SUCCESS(st))
            st = iosb.Status;
    }
    DVDP_TRACE("  [trace] SCSI CDB %02X %02X %02X .. %02X %02X %s %lu bytes = 0x%08X\n",
               req->Cdb[0], req->Cdb[1], req->Cdb[2], req->Cdb[7], req->Cdb[8],
               req->DataIn ? "from drive" : "to drive", req->DataLength, (unsigned)st);
    DVDP_TRACE("  [trace]   sense %02X key %X ASC %02X ASCQ %02X, returned %lu bytes\n",
               sense[0], sense[2] & 0x0F, sense[12], sense[13], iosb.Information);
    return st;
}

static void OddInitRequest(ODD_SCSI_REQUEST* req, int dataIn, void* buffer, unsigned long length)
{
    memset(req, 0, sizeof(*req));
    req->Length     = sizeof(ODD_SCSI_REQUEST);
    req->DataIn     = (unsigned char)dataIn;
    req->DataLength = length;
    req->DataBuffer = buffer;
}

// --- MODE SELECT(10): write one mode page to the drive -----------------------
static DVDP_STATUS OddModeSelect(void* handle, unsigned char pageCode, unsigned char pageLength,
                                 const unsigned char key[DVDP_DVD_KEY_SIZE],
                                 unsigned short modeDataLength)
{
    ODD_MODE_PAGE page;
    memset(&page, 0, sizeof(page));
    page.ModeDataLength = modeDataLength;
    page.PageCode       = pageCode;
    page.PageLength     = pageLength;
    memcpy(page.Data, key, DVDP_DVD_KEY_SIZE);

    unsigned long length = (unsigned long)modeDataLength + 2;
    ODD_SCSI_REQUEST req;
    OddInitRequest(&req, 0, &page, length);
    req.Cdb[0] = SCSI_MODE_SELECT10;
    req.Cdb[7] = (unsigned char)(length >> 8);
    req.Cdb[8] = (unsigned char)length;

    return OddIoctl(handle, &req);
}

// --- MODE SENSE(10): read the 16-byte drive key page -------------------------
static DVDP_STATUS OddReadKeyPage(void* handle, unsigned char outKey[DVDP_DVD_KEY_SIZE])
{
    ODD_MODE_PAGE page;
    memset(&page, 0, sizeof(page));
    page.ModeDataLength = ODD_MODE_DATA_LENGTH_SHORT;

    unsigned long length = ODD_MODE_DATA_LENGTH_SHORT + 2;
    ODD_SCSI_REQUEST req;
    OddInitRequest(&req, 1, &page, length);
    req.Cdb[0] = SCSI_MODE_SENSE10;
    req.Cdb[2] = ODD_PAGE_SENSE_DRIVE_KEY;
    req.Cdb[7] = (unsigned char)(length >> 8);
    req.Cdb[8] = (unsigned char)length;

    DVDP_STATUS st = OddIoctl(handle, &req);
    if (DVDP_SUCCESS(st))
        memcpy(outKey, page.Data, DVDP_DVD_KEY_SIZE);
    return st;
}

// Unlock (BB/30 + 16x0xFF) then set (BB/30 + key) (recovery sub_920D7B80).
static DVDP_STATUS OddWriteKey(void* handle, const unsigned char key[DVDP_DVD_KEY_SIZE])
{
    DVDP_STATUS st = OddModeSelect(handle, ODD_PAGE_SELECT_SET_KEY, 0x30, kUnlockKey,
                                   ODD_MODE_DATA_LENGTH_SET_KEY);
    if (DVDP_SUCCESS(st))
        st = OddModeSelect(handle, ODD_PAGE_SELECT_SET_KEY, 0x30, key,
                           ODD_MODE_DATA_LENGTH_SET_KEY);
    return st;
}

// A5/10 authenticate with a 16-byte key (recovery sub_920D7DD8).
static DVDP_STATUS OddAuthenticateKey(void* handle, const unsigned char key[DVDP_DVD_KEY_SIZE])
{
    return OddModeSelect(handle, ODD_PAGE_SELECT_AUTHENTICATE, 0x10, key,
                         ODD_MODE_DATA_LENGTH_SHORT);
}

//==============================================================================
// Drive-side public helpers.
//==============================================================================

DVDP_STATUS Odd_SetDriveKeyMethod(const unsigned char key[DVDP_DVD_KEY_SIZE],
                                  DVDP_ODD_METHOD method)
{
    void* h = 0;
    DVDP_STATUS st = OddOpen(&h);
    if (!DVDP_SUCCESS(st))
        return st;                                   // "Didn't find a DVD Drive"

    st = OddWriteKey(h, key);

    // NEW method additionally authenticates with the key, matching the extra handshake
    // the recovery's new-console path does for D4S.
    if (DVDP_SUCCESS(st) && method == DVDP_ODD_NEW)
        st = OddAuthenticateKey(h, key);

    NtClose(h);
    return st;                                        // caller logs "Failed to ... ODD"
}

DVDP_STATUS Odd_ReadDriveKey(unsigned char outKey[DVDP_DVD_KEY_SIZE])
{
    void* h = 0;
    DVDP_STATUS st = OddOpen(&h);
    if (!DVDP_SUCCESS(st))
        return st;

    st = OddReadKeyPage(h, outKey);                   // recovery sub_920D7EB8
    NtClose(h);
    return st;
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
    //   "HL-DT-ST" "DVD-ROM GDR3120L" rev 0046/0047/0058/0059/0061/0078/0079   (Hitachi-LG)
    //   "TSSTcorp" "DVD-ROM TS-H943A" rev ms25/ms28                       (Samsung)
    //   "PBDS    " "VAD6038-64930C"   (no rev)                            (BenQ)
    //   "PLDS    " "DG-16D2S"         rev 7485/8385/9345                  (Lite-On DG-16D2S)
    //   "PLDS    " "DG-16D4S"         rev 9504  (+ "J504" from 11775.3 recovery table)
    //   "SATA    " "DVD-ROM"          rev 6243  (generic SATA fallback)
    //   "HL-DT-ST" "DVD-ROM DL10N"    (Hitachi-LG slim)
    // Note the vendors are distinct: BenQ = "PBDS", Lite-On = "PLDS". DG-16D5S isn't in
    // either kernel's table. The extra GDR3120L, BenQ, D2S, D4S, D5S and DL10N revisions in
    // IsKnownFirmware are user-supplied, not from these tables.

    // These models can have different controller chips running D2S firmware, so the model
    // doesn't tell us the pairing method. Matched first, whatever the vendor string says.
    if (strstr(di->Product, "DG-16D5S")) return DVD_FAMILY_LITEON_D5S;
    if (strstr(di->Product, "DG-16D4S")) return DVD_FAMILY_LITEON_D4S;
    if (strstr(di->Product, "DL10N"))    return DVD_FAMILY_HITACHI_DL10N;

    // BenQ (Philips/BenQ lineage): normally vendor "PBDS" product "VAD6038-64930C".
    // Confirmed on hardware: the firmware-"6243" units report generically as vendor
    // "SATA" product "DVD-ROM" instead (the kernel lists that as a distinct 28-char
    // record right after the VAD6038 one). Treat both as BenQ-class. Older units may
    // also read "PHILIPS"/"BENQ".
    if (!strncmp(di->Vendor, "PBDS", 4) || !strncmp(di->Vendor, "BENQ", 4) ||
        !strncmp(di->Vendor, "PHILIPS", 7) || strstr(di->Product, "VAD6038") ||
        (!strncmp(di->Vendor, "SATA", 4) &&
         (!strncmp(di->Revision, "6243", 4) || strstr(di->Product, "DVD-ROM"))))
        return DVD_FAMILY_BENQ;
    // Lite-On: vendor "PLDS" (older units "LITE-ON").
    if (!strncmp(di->Vendor, "PLDS", 4) || !strncmp(di->Vendor, "LITE-ON", 7)) {
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
    static const char* hitachi[] = {"0027","0032","0036","0037","0040","0046","0047","0058",
                                    "0059","0061","0078","0079",0};
    static const char* samsung[] = {"ms25","ms28",0};
    static const char* d2s[]     = {"7485","8385","9345","0251",0};
    static const char* d4s[]     = {"9504","0225","0272","0401","1071","J504",0};
    static const char* d5s[]     = {"1175","1532",0};
    static const char* dl10n[]   = {"0500","0502",0};
    static const char* benq[]    = {"6243","6493","0442","0443",0};  // + VAD6038 matched by product
    switch (fam) {
        case DVD_FAMILY_HITACHI:    list = hitachi; break;
        case DVD_FAMILY_SAMSUNG:    list = samsung; break;
        case DVD_FAMILY_LITEON_D2S: list = d2s;     break;
        case DVD_FAMILY_LITEON_D4S: list = d4s;     break;
        case DVD_FAMILY_LITEON_D5S: list = d5s;     break;
        case DVD_FAMILY_HITACHI_DL10N: list = dl10n; break;
        case DVD_FAMILY_BENQ:
            if (strstr(product, "VAD6038")) return 1;
            list = benq; break;
        default: return 0;          // UNKNOWN family -> firmware not "known-good"
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

    // INQUIRY, CDB 12 00 00 00 24 C0 with a 36-byte buffer. [kernel SataCdRomInitialize,
    // recovery sub_920D7CA8] Vendor/product/revision sit at response offsets 8..35 (the
    // exact 28-byte field the recovery matches).
    __declspec(align(4)) unsigned char resp[36];
    memset(resp, 0, sizeof(resp));

    ODD_SCSI_REQUEST req;
    OddInitRequest(&req, 1, resp, sizeof(resp));
    req.Cdb[0] = SCSI_INQUIRY;
    req.Cdb[4] = sizeof(resp);
    req.Cdb[5] = 0xC0;

    st = OddIoctl(h, &req);
    NtClose(h);
    if (!DVDP_SUCCESS(st))
        return st;

    memcpy(outInfo->Vendor,   resp + 8,  8);  outInfo->Vendor[8]   = 0;
    memcpy(outInfo->Product,  resp + 16, 16); outInfo->Product[16] = 0;
    memcpy(outInfo->Revision, resp + 32, 4);  outInfo->Revision[4] = 0;
    memcpy(outInfo->Inquiry28, resp + 8, 28); outInfo->Inquiry28[28] = 0;

    outInfo->Family = ClassifyDrive(outInfo);
    outInfo->AskMethod = (outInfo->Family == DVD_FAMILY_LITEON_D4S ||
                          outInfo->Family == DVD_FAMILY_LITEON_D5S ||
                          outInfo->Family == DVD_FAMILY_HITACHI_DL10N);
    outInfo->KnownFirmware = IsKnownFirmware(outInfo->Family, outInfo->Revision,
                                             outInfo->Product);
    return st;
}

//==============================================================================
// Console-side (KeyVault) public helper.
//==============================================================================

#ifdef DVDP_TRACE_ENABLED
// Show what this console's hypervisor says about a key, and try reading it. The console
// serial (0x14) is the control: a stock hypervisor allows reading it but not the DVD key.
static void TraceKeyAccess(unsigned long index, unsigned long size)
{
    unsigned char buf[32];
    unsigned long len = size;
    unsigned long props = XeKeysGetKeyProperties(index);
    long st = XeKeysGetKey(index, buf, &len);
    DVDP_TRACE("  [trace] Key 0x%02X: properties 0x%08lX, XeKeysGetKey = 0x%08X, length %lu\n",
               index, props, (unsigned)st, len);
}
#endif

DVDP_STATUS Dvd_GetConsoleKey(unsigned char outKey[DVDP_DVD_KEY_SIZE])
{
#ifdef DVDP_TRACE_ENABLED
    DVDP_TRACE("  [trace] Kernel %u.%u.%u.%u\n", XboxKrnlVersion.Major, XboxKrnlVersion.Minor,
               XboxKrnlVersion.Build, XboxKrnlVersion.Qfe);
    TraceKeyAccess(0x14, 12);
#endif
    unsigned long len = DVDP_DVD_KEY_SIZE;
    DVDP_STATUS st = XeKeysGetKey(KV_INDEX_DVD_KEY, outKey, &len);
    DVDP_TRACE("  [trace] Dvd_GetConsoleKey: XeKeysGetKey(0x%02X) = 0x%08X, length %lu\n",
               KV_INDEX_DVD_KEY, (unsigned)st, len);
#ifdef DVDP_TRACE_ENABLED
    DVDP_TRACE("  [trace] Key 0x%02X: properties 0x%08lX\n", KV_INDEX_DVD_KEY,
               XeKeysGetKeyProperties(KV_INDEX_DVD_KEY));
    if (DVDP_SUCCESS(st)) {
        // Say whether the key came back blank, without printing it.
        int zero = 1, ff = 1;
        for (int i = 0; i < DVDP_DVD_KEY_SIZE; ++i) {
            if (outKey[i] != 0x00) zero = 0;
            if (outKey[i] != 0xFF) ff = 0;
        }
        DVDP_TRACE("  [trace] Console key contents: %s\n",
                   zero ? "ALL ZERO" : ff ? "ALL FF" : "non-blank");
    }
#endif
    return st;
}

//==============================================================================
// High-level flow.
//==============================================================================

DVDP_STATUS DvdPair_PairConnectedDrive(DVDP_ODD_METHOD method)
{
    DVD_DRIVE_INFO di;
    DVDP_STATUS st = Odd_DetectDrive(&di);
    if (!DVDP_SUCCESS(st))
        return st;                       // no drive / INQUIRY failed

    if (method == DVDP_ODD_AUTO) {
        // Don't guess for a model that can use either method (checked first, because these
        // can also have unrecognized firmware) or an unrecognized firmware revision. Report
        // it so the caller can ask the user and re-call. Nothing written yet.
        if (di.AskMethod)
            return DVDP_STATUS_CHOOSE_METHOD;
        if (!di.KnownFirmware)
            return DVDP_STATUS_UNKNOWN_FIRMWARE;
        method = DVDP_ODD_OLD;
    }
    // (An explicit DVDP_ODD_OLD / _NEW overrides all of the above.)

    unsigned char key[DVDP_DVD_KEY_SIZE];
    st = Dvd_GetConsoleKey(key);
    if (!DVDP_SUCCESS(st))
        return st;
    return Odd_SetDriveKeyMethod(key, method);
}
