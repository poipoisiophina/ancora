@echo off
rem Builds ancora and packs it into dist\ancora-setup-<version>.exe with Inno
rem Setup (open source, https://jrsoftware.org/isinfo.php). ISCC.exe is found
rem on PATH, then in the per user and machine wide Inno Setup 7 folders, then in
rem tools\innosetup. When none exists, the compiler is fetched there from its
rem NuGet package (pinned version and SHA-256), so nothing gets installed.
setlocal
cd /d %~dp0
set "INNO_VER=7.1.0"
set "INNO_SHA=aad15c662593c6f5656457a5c001f4591babd72820cacf63f9706d4af5d93b75"
set "INNO_DIR=%~dp0tools\innosetup"
rem full path: cmd may be set not to run scripts from the current folder
call "%~dp0build_driver.bat" all || exit /b 1

set "ISCC="
for /f "delims=" %%i in ('where ISCC.exe 2^>nul') do if not defined ISCC set "ISCC=%%i"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 7\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 7\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe"
rem the marker file names the fetched version, so a version change fetches again
if not defined ISCC if not exist "%INNO_DIR%\%INNO_VER%.ok" call :fetch_inno || exit /b 1
if not defined ISCC set "ISCC=%INNO_DIR%\tools\ISCC.exe"
if exist dist rmdir /s /q dist
"%ISCC%" /Q packaging\ancora.iss || exit /b 1
for %%f in (dist\*.exe) do echo built %%f
exit /b 0

rem the .nupkg is a zip; curl and tar ship with Windows 10 and later
:fetch_inno
echo fetching Inno Setup %INNO_VER% compiler into tools\innosetup
if exist "%INNO_DIR%" rmdir /s /q "%INNO_DIR%"
mkdir "%INNO_DIR%" || exit /b 1
set "PKG=%INNO_DIR%\innosetup.nupkg"
curl -sSfL -o "%PKG%" "https://api.nuget.org/v3-flatcontainer/tools.innosetup/%INNO_VER%/tools.innosetup.%INNO_VER%.nupkg" || (echo download failed & exit /b 1)
set "GOT="
for /f "skip=1 delims=" %%h in ('certutil -hashfile "%PKG%" SHA256') do if not defined GOT set "GOT=%%h"
set "GOT=%GOT: =%"
if /i not "%GOT%"=="%INNO_SHA%" (
    echo checksum mismatch for the Inno Setup package, not using it
    rmdir /s /q "%INNO_DIR%"
    exit /b 1
)
tar -xf "%PKG%" -C "%INNO_DIR%" tools || exit /b 1
del "%PKG%"
type nul > "%INNO_DIR%\%INNO_VER%.ok"
exit /b 0
