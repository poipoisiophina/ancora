@echo off
rem Builds the ancora driver with MSVC (the driver interface needs the MSVC
rem C++ ABI; MinGW mismatches struct-return vcalls).
rem   build_driver.bat         logic only: driver\ancora\logic\ancora_logic.dll
rem                            (hot reloaded by the running driver, no restart)
rem   build_driver.bat all     also the loader driver_ancora.dll (needs SteamVR closed),
rem                            the panel and the probe/reader tools in src\bin
rem Fetches the OpenVR SDK into external\openvr first if it is missing.
setlocal
cd /d %~dp0
if not exist external\openvr\headers\openvr_driver.h (
    echo fetching OpenVR SDK into external\openvr
    where git >nul 2>nul || (echo git not found, install git or place the SDK in external\openvr & exit /b 1)
    git clone --depth 1 https://github.com/ValveSoftware/openvr.git external\openvr || exit /b 1
)
rem locate MSVC with vswhere, which Visual Studio installs under %ProgramFiles(x86)%
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere not found, install Visual Studio Build Tools with C++ & exit /b 1)
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (echo no Visual Studio install with the C++ x64 tools found & exit /b 1)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem start from clean outputs: intermediates always, every built binary for "all"
if exist driver\obj rmdir /s /q driver\obj
if /i "%1"=="all" (
    if exist driver\ancora\bin rmdir /s /q driver\ancora\bin
    if exist driver\ancora\logic rmdir /s /q driver\ancora\logic
    if exist gui\bin rmdir /s /q gui\bin
    if exist src\bin rmdir /s /q src\bin
    echo removed old build outputs
)
if not exist driver\obj mkdir driver\obj
if not exist driver\ancora\logic mkdir driver\ancora\logic
set CL_OPTS=/nologo /std:c++17 /O2 /EHsc /MT /LD /W3 /Iexternal\openvr\headers /Fodriver\obj\
rem build to obj first, then copy, so the loader never sees a half written dll
cl %CL_OPTS% driver\src\ancora_logic.cpp /Fe:driver\obj\ancora_logic.dll /link /NOIMPLIB /NOEXP || exit /b 1
copy /y driver\obj\ancora_logic.dll driver\ancora\logic\ancora_logic.dll >nul || exit /b 1
echo built driver\ancora\logic\ancora_logic.dll
rem deploy to the installed driver too, so the running loader hot reloads it
set INSTALLED=%LOCALAPPDATA%\ancora\driver\ancora\logic
if exist "%INSTALLED%" copy /y driver\obj\ancora_logic.dll "%INSTALLED%\ancora_logic.dll" >nul && echo deployed to %INSTALLED%
if /i not "%1"=="all" exit /b 0
if not exist driver\ancora\bin\win64 mkdir driver\ancora\bin\win64
cl %CL_OPTS% driver\src\driver_ancora.cpp /Fe:driver\obj\driver_ancora.dll /link /NOIMPLIB /NOEXP || exit /b 1
copy /y driver\obj\driver_ancora.dll driver\ancora\bin\win64\driver_ancora.dll >nul || exit /b 1
echo built driver\ancora\bin\win64\driver_ancora.dll
rem control panel (no OpenVR dependency)
if not exist gui\bin mkdir gui\bin
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /DUNICODE /D_UNICODE /Fodriver\obj\ ^
   gui\src\ancora_panel.cpp /Fe:gui\bin\ancora.exe ^
   /link /SUBSYSTEM:WINDOWS || exit /b 1
rem the panel draws its own icon: write it as an .ico, then relink with it embedded
rem as resource 1, so Explorer and Settings > Apps show it (the installer uses it too)
gui\bin\ancora.exe --icon-ico "%~dp0driver\obj\ancora.ico" || exit /b 1
> driver\obj\ancora.rc echo 1 ICON "ancora.ico"
pushd driver\obj
rc /nologo /fo ancora.res ancora.rc || (popd & exit /b 1)
popd
link /nologo /SUBSYSTEM:WINDOWS /OUT:gui\bin\ancora.exe driver\obj\ancora_panel.obj driver\obj\ancora.res ^
   d2d1.lib dwrite.lib windowscodecs.lib shell32.lib user32.lib gdi32.lib ole32.lib || exit /b 1
copy /y gui\ancora.vrmanifest gui\bin\ >nul
echo built gui\bin\ancora.exe
rem diagnostic tools: they use the OpenVR client api, so the SDK's openvr_api.dll
rem goes next to them, with the reader's manifest and bindings
if exist src\bin rmdir /s /q src\bin
mkdir src\bin\bindings
for %%t in (probe reader) do (
    cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /Iexternal\openvr\headers /Fodriver\obj\ ^
       src\%%t.cpp /Fe:src\bin\%%t.exe /link external\openvr\lib\win64\openvr_api.lib || exit /b 1
)
copy /y external\openvr\bin\win64\openvr_api.dll src\bin\ >nul || exit /b 1
copy /y ancora.vrmanifest src\bin\ >nul || exit /b 1
copy /y bindings\*.json src\bin\bindings\ >nul || exit /b 1
echo built src\bin\probe.exe, src\bin\reader.exe
