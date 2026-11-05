@echo off
rem ==========================================================================
rem  build.bat - produce PairDvd.xex for the DVD Drive Pairing Tool using the
rem  Xbox 360 SDK command-line compiler (no Visual Studio).
rem
rem  XexTool (tools) is vendored in this repo. Run AtgFramework-installer.bat once first:
rem  it sets up the ATG framework (atg) from the Xbox 360 SDK (XDK) and builds its lib.
rem  Prereqs / overridable vars:
rem    - XEDK    : Xbox 360 SDK install.
rem    - ATG_DIR : ATG framework (default: atg; needs Common\*.h and
rem                Common\Release_LTCG\AtgFramework.lib).
rem    - XEXTOOL : xorloser XexTool.exe (default: vendored tools\XexTool.exe).
rem  Usage:  build.bat          debug build: unoptimized, with a .pdb
rem          build.bat release  optimized, no symbols
rem ==========================================================================
setlocal enableextensions
pushd "%~dp0"

if "%XEDK%"=="" set "XEDK=C:\Program Files (x86)\Microsoft Xbox 360 SDK"
rem ATG framework lives under atg. Override %ATG_DIR% to use an external
rem checkout instead (must contain Common\*.h and Common\Release_LTCG\AtgFramework.lib).
if "%ATG_DIR%"=="" set "ATG_DIR=%~dp0atg"
if "%XEXTOOL%"=="" set "XEXTOOL=%~dp0tools\XexTool.exe"

set "OUTDIR=bin"
rem Output base name. Note: the 360 auto-launches a title folder's entry point as
rem "default.xex"; a differently-named xex must be launched explicitly (file browser /
rem launcher with a path). Set NAME=Default if you want folder auto-launch.
set "NAME=PairDvd"

rem Xbox 360 title build: use the XBOX includes/libs only (NOT include\win32, which
rem is for host tools and whose xnamath.h rejects Xbox 360).
set "PATH=%XEDK%\bin\win32;%PATH%"
set "INCLUDE=%XEDK%\include\xbox;%XEDK%\include\xbox\sys"
set "LIB=%XEDK%\lib\xbox"

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set "DEFS=/DNDEBUG /D_XBOX /DXBOX /D_XBOX_CRT_DEPRECATE_INSECURE"
set "INCS=/I"%ATG_DIR%\Common" /I."
rem Debug only changes how our code is built. NDEBUG and /LTCG stay on so it still links
rem against the release libs (the prebuilt LTCG AtgFramework.lib and the SDK's).
rem DVDP_TRACE_ENABLED prints each pairing step and its status.
set "CLCONFIG=/Zi /Od /DDVDP_TRACE_ENABLED /Fd"%OUTDIR%\%NAME%_cl.pdb""
set "LINKCONFIG=/DEBUG /PDB:"%OUTDIR%\%NAME%.pdb" /OPT:REF /OPT:NOICF"
if /i "%~1"=="release" (
  set "CLCONFIG=/O2 /GL"
  set "LINKCONFIG=/RELEASE /OPT:REF /OPT:ICF"
)
set "CLFLAGS=/nologo /c %CLCONFIG% /EHsc /GF /Gy %DEFS% %INCS%"

echo [1/4] Compiling...
cl %CLFLAGS% /Fo"%OUTDIR%\main.obj"          main.cpp          || goto :fail
cl %CLFLAGS% /Fo"%OUTDIR%\OutputConsole.obj" OutputConsole.cpp || goto :fail
cl %CLFLAGS% /Fo"%OUTDIR%\DvdPair.obj"       DvdPair.cpp       || goto :fail

rem /XEX:NO is required: without it link.exe emits a finished XEX as the .exe, and imagexex
rem then ignores /section when given that as /IN. Same as the NAND flasher's project.
echo [2/4] Linking...
set "LIBS=AtgFramework.lib xapilib.lib vcomp.lib d3d9ltcg.lib d3dx9.lib xgraphics.lib xaudio2.lib x3daudioltcg.lib xmcoreltcg.lib xboxkrnl.lib xbdm.lib"
link /nologo /LTCG /subsystem:xbox /XEX:NO %LINKCONFIG% ^
  /LIBPATH:"%ATG_DIR%\Common\Release_LTCG" ^
  /OUT:"%OUTDIR%\%NAME%.exe" ^
  "%OUTDIR%\main.obj" "%OUTDIR%\OutputConsole.obj" "%OUTDIR%\DvdPair.obj" %LIBS% || goto :fail

rem /section embeds font.xpr as the XEX resource AtgResource loads as "embed:\font"
rem (XGetModuleSection). Matches the flasher's <AdditionalSections>font=...,RO.
echo [3/4] Building XEX...
imagexex /NOLOGO /IN:"%OUTDIR%\%NAME%.exe" /OUT:"%OUTDIR%\%NAME%_img.xex" /CONFIG:xex.xml /section:font=font.xpr,RO || goto :fail

echo [4/4] Removing restrictions (devkit, unencrypted, uncompressed)...
"%XEXTOOL%" -r a -m d -e u -c u -o "%OUTDIR%\%NAME%.xex" "%OUTDIR%\%NAME%_img.xex" || goto :fail

echo.
echo BUILD OK -^> %OUTDIR%\%NAME%.xex
popd & endlocal & exit /b 0

:fail
echo.
echo BUILD FAILED (errorlevel %errorlevel%)
popd & endlocal & exit /b 1
