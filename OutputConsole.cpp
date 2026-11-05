#include <xtl.h>
#include "AtgConsole.h"
#include "AtgUtil.h"
#include "OutputConsole.h"
#include <stdio.h>
#include <stdarg.h>

#define TEXTBUFFER_SIZE 512

ATG::Console g_console;          // console for output
static char  s_buf[TEXTBUFFER_SIZE];

void __cdecl dprintf(const char* strFormat, ...)
{
    va_list args;
    va_start(args, strFormat);
    vsnprintf_s(s_buf, TEXTBUFFER_SIZE, _TRUNCATE, strFormat, args);
    va_end(args);
    g_console.Display(s_buf);
}

void MakeConsole(const char* font, unsigned long BackgroundColor, unsigned long TextColor)
{
    g_console.Create(font, BackgroundColor, TextColor);
    g_console.SendOutputToDebugChannel(TRUE);
}

void ClearConsole()
{
    g_console.Clear();
}
