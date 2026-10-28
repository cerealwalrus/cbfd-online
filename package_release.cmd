@echo off
setlocal EnableExtensions
rem Makes the release zip from host\build-win with only what players need: the
rem game, the launcher, the DLLs, the assets and the licenses. It never takes
rem conker_data (the ROM, saves and mods) or build files (.pdb, .ilk, .lib,
rem CMakeFiles), and it checks the finished zip for ROM and save files before
rem saying it is done.
rem
rem Usage: package_release.cmd   (after build.cmd; the zip lands in release\)

cd /d "%~dp0"
set "BUILD=host\build-win"
set "OUT=release"

set "VERSION="
for /f "tokens=3" %%V in ('findstr /b /l /c:"#define CONKER_ONLINE_VERSION" host\src\version.hpp') do set "VERSION=%%~V"
if not defined VERSION (
    echo Could not read the version from host\src\version.hpp.
    goto fail
)
set "NAME=CBFD-Online-%VERSION%"
set "STAGE=%OUT%\%NAME%"
set "ZIP=%OUT%\%NAME%.zip"
set "LIST=%OUT%\%NAME%-contents.txt"

for %%F in (ConkerOnline.exe SDL2.dll dxcompiler.dll dxil.dll) do (
    if not exist "%BUILD%\%%F" (
        echo Missing %BUILD%\%%F - run build.cmd first.
        goto fail
    )
)
if not exist "%BUILD%\assets\" (
    echo Missing %BUILD%\assets - run build.cmd first.
    goto fail
)
rem The exe carries its version string: an exe left over from an older build would not have this one.
findstr /m /l /c:"%VERSION%" "%BUILD%\ConkerOnline.exe" >nul
if errorlevel 1 (
    echo %BUILD%\ConkerOnline.exe was not built as version %VERSION% - run build.cmd first.
    goto fail
)

if not exist "%OUT%\" mkdir "%OUT%"
if exist "%STAGE%\" rmdir /s /q "%STAGE%"
if exist "%ZIP%" del /q "%ZIP%"
if exist "%LIST%" del /q "%LIST%"
mkdir "%STAGE%" || goto fail

for %%F in (ConkerOnline.exe SDL2.dll dxcompiler.dll dxil.dll) do (
    copy /y "%BUILD%\%%F" "%STAGE%\" >nul || goto fail
)
robocopy "%BUILD%\assets" "%STAGE%\assets" /E /XF *.z64 *.n64 *.v64 *.rom *.eep *.bin /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto fail
copy /y ".github\release\README.txt" "%STAGE%\" >nul || goto fail
copy /y "LICENSE" "%STAGE%\" >nul || goto fail
copy /y ".github\release\LICENSES.txt" "%STAGE%\" >nul || goto fail
copy /y "tools\N64ModernRuntime\COPYING" "%STAGE%\LICENSE-GPL-3.0.txt" >nul || goto fail

rem tar is built into Windows 10 and 11; -a makes a .zip from the name. The game's updater unpacks it the same way.
tar -a -c -f "%ZIP%" -C "%OUT%" "%NAME%"
if errorlevel 1 goto fail
rmdir /s /q "%STAGE%"

rem Check what actually went into the zip.
tar -t -f "%ZIP%" > "%LIST%"
if errorlevel 1 goto fail
findstr /i /r /c:"\.z64" /c:"\.n64" /c:"\.v64" /c:"\.rom" /c:"\.eep" /c:"\.sav" /c:"conker_data" /c:"\.pdb" /c:"\.ilk" /c:"\.lib" /c:"CMakeFiles" "%LIST%"
if not errorlevel 1 (
    echo.
    echo STOPPED: the files above should not be in a release. The zip was deleted.
    del /q "%ZIP%"
    goto fail
)
del /q "%LIST%"

for %%Z in ("%ZIP%") do set "SIZE=%%~zZ"
echo.
echo Made %ZIP% (%SIZE% bytes): no ROM, saves or build files in it.
echo Upload it to the v%VERSION% release.
exit /b 0

:fail
echo.
echo package_release.cmd did not finish.
exit /b 1
