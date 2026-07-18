@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "CONFIG=Release"

if not "%~1"=="" set "CONFIG=%~1"
if /I not "%CONFIG%"=="Release" if /I not "%CONFIG%"=="Debug" goto :usage
if not "%~3"=="" goto :usage
if not "%~2"=="" if /I not "%~2"=="--clean" goto :usage

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe was not found. Install Visual Studio 2022. 1>&2
    exit /b 2
)

set "VSINSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio 2022 with the x86 C++ toolset was not found. 1>&2
    exit /b 2
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 exit /b %errorlevel%

for %%T in (cl.exe link.exe midl.exe nmake.exe powershell.exe rc.exe) do (
    where %%T >nul 2>&1 || (
        echo ERROR: %%T is unavailable after vcvars32.bat. 1>&2
        exit /b 2
    )
)
if not exist "%VCToolsInstallDir%ATLMFC\lib\spectre\x86\mfc*.lib" (
    echo ERROR: Spectre-mitigated x86 MFC libraries are missing. 1>&2
    echo Add the Visual Studio Individual Component for the current v143 1>&2
    echo C++ MFC Spectre-mitigated libraries, then run this command again. 1>&2
    exit /b 2
)

if /I "%~2"=="--clean" (
    if exist "%ROOT%source\%CONFIG%" rmdir /s /q "%ROOT%source\%CONFIG%"
    if exist "%ROOT%source\%CONFIG%" (
        echo ERROR: Could not remove the stale %CONFIG% output directory. 1>&2
        exit /b 3
    )
)

set "IDLHEADERBACKUP=%TEMP%\ComicChat-icchat-%RANDOM%-%RANDOM%.h"
copy /y "%ROOT%source\icchat.h" "%IDLHEADERBACKUP%" >nul
if errorlevel 1 (
    echo ERROR: Could not preserve the imported icchat.h before MIDL generation. 1>&2
    exit /b 3
)
if exist "%ROOT%source\icchat_i.c" del /q "%ROOT%source\icchat_i.c"

pushd "%ROOT%source" || (
    del /q "%IDLHEADERBACKUP%" >nul 2>&1
    echo ERROR: Could not enter the imported source directory. 1>&2
    exit /b 3
)
nmake /nologo /f chat.mak CFG="chat - Win32 %CONFIG%"
set "RESULT=%errorlevel%"
popd
copy /y "%IDLHEADERBACKUP%" "%ROOT%source\icchat.h" >nul
set "RESTORERESULT=%errorlevel%"
if exist "%ROOT%source\icchat_i.c" del /q "%ROOT%source\icchat_i.c"
if not "%RESTORERESULT%"=="0" (
    echo ERROR: Could not restore the pinned icchat.h after MIDL generation. 1>&2
    echo The preserved header remains at %IDLHEADERBACKUP%. 1>&2
    exit /b 3
)
del /q "%IDLHEADERBACKUP%" >nul 2>&1
if not "%RESULT%"=="0" exit /b %RESULT%

set "EXE=%ROOT%source\%CONFIG%\CChat.exe"
if not exist "%EXE%" (
    echo ERROR: NMAKE completed without producing %EXE%. 1>&2
    exit /b 3
)
for %%F in ("%EXE%") do if %%~zF LSS 4096 (
    echo ERROR: %EXE% is unexpectedly small. 1>&2
    exit /b 3
)

set "PEVALIDATOR=%ROOT%scripts\common.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { . $env:PEVALIDATOR; Assert-ComicChatX86Pe -LiteralPath $env:EXE } catch { Write-Error $_; exit 1 }"
if errorlevel 1 (
    echo ERROR: Build output is not a valid x86 PE executable. 1>&2
    exit /b 3
)

echo Built %EXE%
exit /b 0

:usage
echo Usage: build.cmd [Release^|Debug] [--clean] 1>&2
exit /b 64
