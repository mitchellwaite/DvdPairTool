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
static void DoPair()
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
    dprintf("Drive : %s %s rev %s\n", di.Vendor, di.Product, di.Revision);
    dprintf("Family: %s\n", FamilyName(di.Family));
    dprintf("Firmware: %s\n", di.KnownFirmware ? "known-good" : "UNRECOGNIZED");
    if (di.AskMethod)
        dprintf("NOTE: This model can use either pairing method - you'll be asked to choose.\n");
}

VOID __cdecl main()
{
    MakeConsole("embed:\\font", CONSOLE_COLOR_BLACK, CONSOLE_COLOR_GOLD);

    dprintf("Xbox 360 DVD Drive Pairing Tool\n");
    dprintf("Devkit/testkit or patched-HV retail only.\n");
    dprintf("--------------------------------------------------------------\n");
    ShowDrive();
    dprintf("--------------------------------------------------------------\n");
    dprintf("A = Pair drive to this console (KeyVault key -> drive)\n");
    dprintf("X = Read current drive key\n");
    dprintf("BACK = Exit (reboot)\n");

    for (;;) {
        WORD b = PollButtons();
        if (b & XINPUT_GAMEPAD_A)          DoPair();
        else if (b & XINPUT_GAMEPAD_X)     DoReadKey();
        else if (b & XINPUT_GAMEPAD_BACK)  break;
        Sleep(16);
    }

    HalReturnToFirmware(HalReturnToFirmwareReboot);
}
