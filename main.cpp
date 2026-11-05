//------------------------------------------------------------------------------
// main.cpp - Xbox 360 DVD Drive Pairing Tool (text console app)
//
// Built on the ATG console sample, same pattern as the Simple 360 NAND Flasher.
// Drives the DvdPair module (DvdPair.cpp) to pair a DVD drive to the console.
//
// DEVKIT / TEST KIT, or patched-HV retail (RGH/JTAG). See CLAUDE.md.
//------------------------------------------------------------------------------
#include <xtl.h>
#include "AtgConsole.h"
#include "AtgInput.h"
#include "AtgUtil.h"
#include "OutputConsole.h"

#include "DvdPair.h"   // C-linkage API

extern "C" VOID HalReturnToFirmware(DWORD mode);
#define HalReturnToFirmwareReboot 1

// Kernel version export
struct KRNL_VERSION { WORD Major, Minor, Build, Qfe; };
extern "C" __declspec(dllimport) KRNL_VERSION XboxKrnlVersion;
extern "C" DWORD MmGetPhysicalAddress(void* buffer);

typedef struct _XBOX_HARDWARE_INFO {
	DWORD Flags;
	unsigned char NumberOfProcessors;
	unsigned char PCIBridgeRevisionID;
	unsigned char Reserved[6];
	unsigned short BldrMagic;
	unsigned short BldrFlags;
} XBOX_HARDWARE_INFO;

extern "C" __declspec(dllimport) XBOX_HARDWARE_INFO XboxHardwareInfo;
#define XBOX_HARDWARE_FLAG_NOT_RETAIL   (1 << 4)   // set on devkit/test kernels

// freeboot/RGL syscall 0 backdoor
static int __declspec(naked) HvxGetVersions(unsigned int magic, int op,
                                            unsigned long long source,
                                            unsigned long long dest,
                                            unsigned long long length)
{
    __asm {
        li r0, 0
        sc
        blr
    }
}

// Patch HvxKeysGetKey to allow read access to all keyvault keys. This is present on FreeBoot
// by default but many flavours of RGL do not have this patch
static void PatchDevkitHypervisor()
{
    // Probe the syscall-0 backdoor first (op 1 returns 1 when it's present). If it's missing,
    // the memcpy below would do nothing or fault, so don't attempt the patch.
    if (HvxGetVersions(0x72627472, 1, 0, 0, 0) != 1) {
        dprintf("WARNING: HV syscall backdoor not available; skipping HvxKeysGetKey patch.\n");
        dprintf("--------------------------------------------------------------\n");
        return;
    }

    unsigned char* buf = (unsigned char*)XPhysicalAlloc(0x1000, MAXULONG_PTR, 0, PAGE_READWRITE);
    if (!buf) {
        dprintf("WARNING: could not allocate a buffer for the HV patch.\n");
        return;
    }
    unsigned long long bufPhys = 0x8000000000000000ULL + (unsigned int)MmGetPhysicalAddress(buf);
    *(unsigned int*)buf = 0x48000030;    // ppc: b +0x30
    HvxGetVersions(0x72627472, 5, bufPhys, 0x8000010000000000ULL + 0x7DD4, 4);
    XPhysicalFree(buf);
    dprintf("Successfully applied HvxKeysGetKey patch\n");
    dprintf("--------------------------------------------------------------\n");
}

static void CheckKernel()
{
    BOOL isDevkit = (XboxHardwareInfo.Flags & XBOX_HARDWARE_FLAG_NOT_RETAIL) != 0;
    if (!isDevkit)
        return;
    if (XboxKrnlVersion.Build < 17489) {
        dprintf("WARNING: devkit kernel %u is older than %u; this tool may not be supported.\n",
                XboxKrnlVersion.Build, 17489);
        dprintf("--------------------------------------------------------------\n");
    } else {
        PatchDevkitHypervisor();
    }
}

static ATG::GAMEPAD* g_pad;

static const char* FamilyName(DVD_DRIVE_FAMILY f)
{
    switch (f) {
        case DVD_FAMILY_SAMSUNG:    return "Samsung (TSSTcorp)";
        case DVD_FAMILY_HITACHI:    return "Hitachi-LG";
        case DVD_FAMILY_BENQ:       return "BenQ (PBDS)";
        case DVD_FAMILY_LITEON_D2S: return "Lite-On DG-16D2S";
        case DVD_FAMILY_LITEON_D4S: return "Lite-On DG-16D4S";
        case DVD_FAMILY_LITEON_D5S: return "Lite-On DG-16D5S";
        case DVD_FAMILY_HITACHI_DL10N: return "Hitachi-LG DL10N";
        default:                    return "UNKNOWN";
    }
}

// Poll one fresh input frame (edge-triggered buttons in wPressedButtons).
static WORD PollButtons()
{
    g_pad = ATG::Input::GetMergedInput();
    return g_pad->wPressedButtons;
}

// Wait until all buttons are released, so one press isn't read twice.
static void WaitAllReleased()
{
    for (;;) {
        g_pad = ATG::Input::GetMergedInput();
        if (g_pad->wButtons == 0)
            return;
        Sleep(16);
    }
}

static void ReportPairStatus(DVDP_STATUS st)
{
    if (st >= 0)
        dprintf("  OK (0x%08X)\n", (unsigned)st);
    else
        dprintf("  FAILED (0x%08X)\n", (unsigned)st);
}

// Run a pairing attempt; if it defers to the user, let them pick the method.
// Returns nonzero once an attempt has run (whatever the result), 0 if cancelled.
static int DoPair()
{
    dprintf("\nPairing drive to this console (KeyVault key -> drive)...\n");
    DVDP_STATUS st = DvdPair_PairConnectedDrive(DVDP_ODD_AUTO);

    int chooseMethod = ((unsigned)st == (unsigned)DVDP_STATUS_CHOOSE_METHOD);
    if (chooseMethod || (unsigned)st == (unsigned)DVDP_STATUS_UNKNOWN_FIRMWARE) {
        if (chooseMethod)
            dprintf("  This drive model can use either method - choose one:\n");
        else
            dprintf("  Firmware not in the known list - choose a method:\n");
        dprintf("    X = OLD path (BB/30 unlock + set)\n");
        dprintf("    Y = NEW path (+ A5/10 authenticate)\n");
        dprintf("    B = cancel\n");
        WaitAllReleased();
        for (;;) {
            WORD b = PollButtons();
            if (b & XINPUT_GAMEPAD_X) {
                dprintf("  Trying OLD path...\n");
                st = DvdPair_PairConnectedDrive(DVDP_ODD_OLD); break;
            }
            if (b & XINPUT_GAMEPAD_Y) {
                dprintf("  Trying NEW path...\n");
                st = DvdPair_PairConnectedDrive(DVDP_ODD_NEW); break;
            }
            if (b & XINPUT_GAMEPAD_B) { dprintf("  Cancelled.\n"); WaitAllReleased(); return 0; }
            Sleep(16);
        }
    }
    ReportPairStatus(st);
    WaitAllReleased();
    return 1;
}

// After a pairing attempt the only way on is a reboot, so the console picks up the drive.
static void WaitForRebootPress()
{
    dprintf("\nPress A to reboot.\n");
    for (;;) {
        if (PollButtons() & XINPUT_GAMEPAD_A)
            HalReturnToFirmware(HalReturnToFirmwareReboot);
        Sleep(16);
    }
}

static void DoReadKey()
{
    unsigned char key[DVDP_DVD_KEY_SIZE];
    dprintf("\nReading current drive key...\n");
    DVDP_STATUS st = Odd_ReadDriveKey(key);
    if (st < 0) { dprintf("  FAILED (0x%08X)\n", (unsigned)st); WaitAllReleased(); return; }
    dprintf("  Drive key: ");
    for (int i = 0; i < DVDP_DVD_KEY_SIZE; ++i) dprintf("%02X", key[i]);
    dprintf("\n");
    WaitAllReleased();
}

static void ShowDrive()
{
    DVD_DRIVE_INFO di;
    DVDP_STATUS st = Odd_DetectDrive(&di);
    if (st < 0) {
        dprintf("No DVD drive detected (0x%08X).\n", (unsigned)st);
        return;
    }
    dprintf("Drive : %s %s rev %s\n", di.Vendor, di.Product, di.Revision);
    dprintf("Family: %s\n", FamilyName(di.Family));
    dprintf("Firmware: %s\n", di.KnownFirmware ? "known-good" : "UNRECOGNIZED");
    if (di.AskMethod) {
        dprintf("Caution for slim DVD drives:\n");
        dprintf("- LTU2 drives use the 'old' pairing method\n");
        dprintf("- pairing may fail if the drive has a locked flash.\n");
        dprintf("- drive auth may fail after pairing if an fcrt is in use\n");
    }
}

VOID __cdecl main()
{
    MakeConsole("embed:\\font", CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GOLD);

    dprintf("Xbox 360 DVD Drive Pairing Tool\n");
    dprintf("--------------------------------------------------------------\n");
    CheckKernel();
    ShowDrive();
    dprintf("--------------------------------------------------------------\n");
    dprintf("A = Pair drive to this console (KeyVault key -> drive)\n");
    dprintf("X = Read current drive key\n");
    dprintf("BACK = Exit to dashboard\n");

    for (;;) {
        WORD b = PollButtons();
        if (b & XINPUT_GAMEPAD_A) {
            if (DoPair())
                WaitForRebootPress();
        }
        else if (b & XINPUT_GAMEPAD_X)     DoReadKey();
        else if (b & XINPUT_GAMEPAD_BACK) {
            // Nothing has been written from the main menu, so no reboot is needed.
            XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
            dprintf("Couldn't return to the dashboard.\n");
        }
        Sleep(16);
    }
}
