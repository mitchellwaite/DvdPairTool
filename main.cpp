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

static ATG::GAMEPAD* g_pad;

static const char* FamilyName(DVD_DRIVE_FAMILY f)
{
    switch (f) {
        case DVD_FAMILY_SAMSUNG:    return "Samsung (TSSTcorp)";
        case DVD_FAMILY_HITACHI:    return "Hitachi-LG";
        case DVD_FAMILY_BENQ:       return "BenQ (PBDS)";
        case DVD_FAMILY_LITEON_D2S: return "Lite-On DG-16D2S";
        case DVD_FAMILY_LITEON_D4S: return "Lite-On DG-16D4S";
        case DVD_FAMILY_LITEON_D5S: return "Lite-On DG-16D5S (unsupported)";
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
    else if ((unsigned)st == (unsigned)DVDP_STATUS_UNKNOWN_FIRMWARE)
        dprintf("  Unrecognized firmware.\n");
    else
        dprintf("  FAILED (0x%08X)\n", (unsigned)st);
}

// Run a pairing attempt; on unknown firmware, let the user pick OLD or NEW.
static void DoPair(int fullRepair)
{
    DVDP_STATUS st;
    if (fullRepair) {
        dprintf("\nFull re-pair: generating a NEW key and writing it to BOTH the\n");
        dprintf("KeyVault and the drive. Make sure you have a KeyVault backup!\n");
        st = DvdPair_RepairWithNewKey();
    } else {
        dprintf("\nPairing drive to this console (KeyVault key -> drive)...\n");
        st = DvdPair_PairConnectedDriveEx(NULL /*use console key*/, 0, DVDP_ODD_AUTO);
    }

    if ((unsigned)st == (unsigned)DVDP_STATUS_UNKNOWN_FIRMWARE) {
        dprintf("  Firmware not in the known list - choose a method:\n");
        dprintf("    X = OLD path (BB/30 unlock + set)\n");
        dprintf("    Y = NEW path (+ A5/10 authenticate)\n");
        dprintf("    B = cancel\n");
        WaitAllReleased();
        for (;;) {
            WORD b = PollButtons();
            if (b & XINPUT_GAMEPAD_X) {
                dprintf("  Trying OLD path...\n");
                st = DvdPair_PairConnectedDriveEx(NULL, 0, DVDP_ODD_OLD); break;
            }
            if (b & XINPUT_GAMEPAD_Y) {
                dprintf("  Trying NEW path...\n");
                st = DvdPair_PairConnectedDriveEx(NULL, 0, DVDP_ODD_NEW); break;
            }
            if (b & XINPUT_GAMEPAD_B) { dprintf("  Cancelled.\n"); return; }
            Sleep(16);
        }
    }
    ReportPairStatus(st);
    WaitAllReleased();
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
    dprintf("Drive : [%s] [%s] rev [%s]\n", di.Vendor, di.Product, di.Revision);
    dprintf("Family: %s\n", FamilyName(di.Family));
    dprintf("Firmware: %s\n", di.KnownFirmware ? "known-good" : "UNRECOGNIZED");
    if (di.Family == DVD_FAMILY_LITEON_D5S)
        dprintf("NOTE: DG-16D5S flash is not writable - cannot set the key.\n");
}

VOID __cdecl main()
{
    MakeConsole("embed:\\font", CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GOLD);

    dprintf("Xbox 360 DVD Drive Pairing Tool\n");
    dprintf("Devkit/testkit or patched-HV retail only. Back up your KeyVault first.\n");
    dprintf("--------------------------------------------------------------\n");
    ShowDrive();
    dprintf("--------------------------------------------------------------\n");
    dprintf("A = Pair drive to this console (KeyVault key -> drive)\n");
    dprintf("X = Read current drive key\n");
    dprintf("Y = Full re-pair (NEW key to KeyVault + drive) [needs backup]\n");
    dprintf("BACK = Exit (reboot)\n");

    for (;;) {
        WORD b = PollButtons();
        if (b & XINPUT_GAMEPAD_A)          DoPair(0);
        else if (b & XINPUT_GAMEPAD_X)     DoReadKey();
        else if (b & XINPUT_GAMEPAD_Y)     DoPair(1);
        else if (b & XINPUT_GAMEPAD_BACK)  break;
        Sleep(16);
    }

    HalReturnToFirmware(HalReturnToFirmwareReboot);
}
