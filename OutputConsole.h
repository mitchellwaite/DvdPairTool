#pragma once
// Minimal text console wrapper around ATG::Console (from the ATG framework), modeled
// on the Simple 360 NAND Flasher's OutputConsole. ASCII only, no file logging.

#define CONSOLE_COLOR_BLACK   0xFF000000
#define CONSOLE_COLOR_WHITE   0xFFFFFFFF
#define CONSOLE_COLOR_RED     0xFFFF0000
#define CONSOLE_COLOR_GREEN   0xFF00C000
#define CONSOLE_COLOR_YELLOW  0xFFFFF700
#define CONSOLE_COLOR_GRAY    0xFF808080
#define CONSOLE_COLOR_GOLD    0xFFFFD700
#define CONSOLE_COLOR_CYAN    0xFF00FFFF

#ifdef __cplusplus
extern "C" {
#endif
void __cdecl dprintf(const char* strFormat, ...);
#ifdef __cplusplus
}
#endif

void MakeConsole(const char* font, unsigned long BackgroundColor, unsigned long TextColor);
void ClearConsole();
