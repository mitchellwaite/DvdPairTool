@echo off
rem ==========================================================================
rem  AtgFramework-installer.bat - set up the ATG framework in atg\Common: copy
rem  its sources from the Xbox 360 SDK, then build AtgFramework.lib.
rem
rem  Only adds what is missing: existing files are never overwritten, because
rem  AtgConsole.cpp, AtgConsole.h and AtgResource.cpp in atg\Common carry changes
rem  this project needs (from the Simple 360 NAND Flasher). Those three are tracked
rem  in git; the rest of atg is git-ignored.
rem
rem  The SDK has no prebuilt AtgFramework.lib, so it's built here if missing. Delete
rem  atg\Common\Release_LTCG\AtgFramework.lib to rebuild it after changing a source.
rem ==========================================================================
setlocal enableextensions
cd /d "%~dp0"

rem For 32 bit systems %ProgramFiles(x86)% doesn't exist so we need to check for both.
if "%XEDK%"=="" if exist "%ProgramFiles(x86)%\Microsoft Xbox 360 SDK\Source\Samples\Common\" set "XEDK=%ProgramFiles(x86)%\Microsoft Xbox 360 SDK"
if "%XEDK%"=="" if exist "%ProgramFiles%\Microsoft Xbox 360 SDK\Source\Samples\Common\" set "XEDK=%ProgramFiles%\Microsoft Xbox 360 SDK"
set "SRC=%XEDK%\Source\Samples\Common"
if not exist "%SRC%\" (
  echo Error: Cannot find the XDK! Make sure it is installed.
  goto :fail
)

if not exist "atg\Common" mkdir "atg\Common"

set /a COPIED=0, SKIPPED=0
for %%F in ("%SRC%\*") do (
  if exist "atg\Common\%%~nxF" (
    set /a SKIPPED+=1
  ) else (
    copy /y "%%F" "atg\Common\" >nul && set /a COPIED+=1
  )
)
echo Copied %COPIED% file(s) to atg\Common, kept %SKIPPED% existing.

rem A lib built from the stock files wouldn't work, so check before building it.
findstr /c:"embed:" "atg\Common\AtgResource.cpp" >nul || goto :stock
findstr /c:"Display( LPCSTR" "atg\Common\AtgConsole.h" >nul || goto :stock
findstr /c:"Console::Display" "atg\Common\AtgConsole.cpp" >nul || goto :stock

if exist "atg\Common\Release_LTCG\AtgFramework.lib" (
  echo AtgFramework.lib already exists, not rebuilding.
  goto :done
)

echo Building AtgFramework.lib...
set "PATH=%XEDK%\bin\win32;%PATH%"
set "INCLUDE=%XEDK%\include\xbox;%XEDK%\include\xbox\sys"
set "LIB=%XEDK%\lib\xbox"
pushd "atg\Common"
if not exist "Release_LTCG" mkdir "Release_LTCG"
del /q "Release_LTCG\*.obj" 2>nul

rem Compiler settings from the Release_LTCG configuration of AtgFramework2010.vcxproj,
rem with fewer warnings and no debug info. XTLOnPC.cpp isn't part of that project.
set "CLFLAGS=/nologo /c /Ox /Oi /Os /GL /GF /Gy /GS- /GR- /MT /W1 /Zc:forScope /DNDEBUG /D_XBOX /DXBOX /DLTCG /D_XBOX_CRT_DEPRECATE_INSECURE /D_MBCS /FpRelease_LTCG\AtgFramework.pch /FoRelease_LTCG\"
cl %CLFLAGS% /Yc"stdafx.h" stdafx.cpp || goto :buildfail

set "RSP=Release_LTCG\sources.rsp"
if exist "%RSP%" del "%RSP%"
for %%F in (*.cpp) do if /i not "%%F"=="stdafx.cpp" if /i not "%%F"=="XTLOnPC.cpp" echo %%F>>"%RSP%"
cl %CLFLAGS% /MP /Yu"stdafx.h" @"%RSP%" || goto :buildfail

lib /nologo /LTCG /OUT:Release_LTCG\AtgFramework.lib Release_LTCG\*.obj || goto :buildfail
popd
echo AtgFramework.lib built.

:done
pause
exit /b 0

:stock
echo Error: AtgConsole.cpp, AtgConsole.h or AtgResource.cpp in atg\Common is the stock SDK file.
echo Restore them with git before building: git checkout -- atg/Common
goto :fail

:buildfail
popd
echo Error: Building AtgFramework.lib failed.

:fail
pause
exit /b 1
