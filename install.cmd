@echo off
rem ============================================================================
rem  install.cmd - Install the freshly built Ext2Fsd / Ext4Fsd driver.
rem
rem  Self-elevates to Administrator, then:
rem    1. installs the WDK test certificate (Root + TrustedPublisher),
rem    2. enables boot test-signing (test-signed kernel drivers need this),
rem    3. installs the driver via its INF (-> %SystemRoot%\System32\drivers),
rem    4. installs the Ext2Srv helper service and starts the driver.
rem
rem  Usage:   install.cmd [Configuration] [Platform]
rem           Configuration : Debug (default) | Release
rem           Platform      : x64 (default) | Win32/x86 | ARM | ARM64
rem
rem  Defaults match build.cmd. Binary path uses the same OutPlatform map
rem  (Win32->x86, ARM->arm, ARM64->arm64).
rem
rem  Run build.cmd first to produce the binaries.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

rem --- self-elevate -----------------------------------------------------------
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator privileges...
    if "%~1"=="" (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    ) else (
        powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    )
    exit /b
)

set "ROOT=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"

if /i "%CONFIG%"=="debug" set "CONFIG=Debug"
if /i "%CONFIG%"=="release" set "CONFIG=Release"
if /i not "%CONFIG%"=="Debug" if /i not "%CONFIG%"=="Release" (
    echo ERROR: Unknown configuration "%CONFIG%". Use Debug or Release.
    goto :fail
)

if /i "%PLATFORM%"=="x86" set "PLATFORM=Win32"
if /i "%PLATFORM%"=="win32" set "PLATFORM=Win32"
if /i "%PLATFORM%"=="x64" set "PLATFORM=x64"
if /i "%PLATFORM%"=="arm" set "PLATFORM=ARM"
if /i "%PLATFORM%"=="arm64" set "PLATFORM=ARM64"
if /i not "%PLATFORM%"=="x64" if /i not "%PLATFORM%"=="Win32" if /i not "%PLATFORM%"=="ARM" if /i not "%PLATFORM%"=="ARM64" (
    echo ERROR: Unknown platform "%PLATFORM%". Use x64, Win32/x86, ARM, or ARM64.
    goto :fail
)

set "OUT_PLATFORM=%PLATFORM%"
if /i "%PLATFORM%"=="Win32" set "OUT_PLATFORM=x86"
if /i "%PLATFORM%"=="ARM" set "OUT_PLATFORM=arm"
if /i "%PLATFORM%"=="ARM64" set "OUT_PLATFORM=arm64"

set "DRV=%ROOT%Ext4Fsd\%CONFIG%\%OUT_PLATFORM%\Ext2Fsd.sys"
set "INF=%ROOT%Ext4Fsd\Ext2Fsd.inf"
set "CER=%ROOT%Setup\Ext2Fsd.cer"
set "MGR=%ROOT%Ext2Mgr\%CONFIG%\%OUT_PLATFORM%\Ext2Mgr.exe"
set "SRV=%ROOT%Ext2Srv\%CONFIG%\%OUT_PLATFORM%\Ext2Srv.exe"
set "INSTDIR=%ProgramFiles%\Ext2Fsd"
set "EXITCODE=0"
set "NEED_REBOOT="
set "SCHEDULE_OK="

echo.
echo === Ext2Fsd install : %CONFIG%^|%PLATFORM% (out %OUT_PLATFORM%) ===
echo.

if not exist "%DRV%" (
    echo ERROR: driver not found:
    echo   %DRV%
    echo Run build.cmd %CONFIG% %PLATFORM% first.
    goto :fail
)
if not exist "%CER%" (
    echo ERROR: test certificate not found: %CER%
    goto :fail
)

rem --- stop any previous instance --------------------------------------------
echo Stopping any previous instance...
net stop ext2srv   >nul 2>&1
net stop Ext2Fsd   >nul 2>&1
sc stop Ext2Fsd    >nul 2>&1

rem --- stage files ------------------------------------------------------------
echo Staging files to "%INSTDIR%" ...
if not exist "%INSTDIR%" mkdir "%INSTDIR%"
copy /y "%DRV%" "%INSTDIR%\Ext2Fsd.sys" >nul || goto :copyfail
copy /y "%INF%" "%INSTDIR%\Ext2Fsd.inf" >nul || goto :copyfail
copy /y "%CER%" "%INSTDIR%\Ext2Fsd.cer" >nul || goto :copyfail
if exist "%MGR%" copy /y "%MGR%" "%INSTDIR%\Ext2Mgr.exe" >nul
if exist "%SRV%" copy /y "%SRV%" "%INSTDIR%\Ext2Srv.exe" >nul

rem --- install the test certificate ------------------------------------------
echo Installing test certificate...
certutil -addstore -f Root            "%INSTDIR%\Ext2Fsd.cer" >nul
certutil -addstore -f TrustedPublisher "%INSTDIR%\Ext2Fsd.cer" >nul

rem --- enable boot test-signing ----------------------------------------------
set "TS_WAS_ON="
bcdedit | findstr /i "testsigning" | findstr /i "Yes" >nul 2>&1 && set "TS_WAS_ON=1"
if defined TS_WAS_ON (
    echo Test-signing already enabled.
) else (
    echo Enabling boot test-signing...
    bcdedit /set testsigning on
)

rem --- register service + registry parameters via INF ------------------------
rem  InstallHinfSection sets up the service and the Parameters/EventLog regkeys.
rem  It will NOT reliably copy the .sys into System32\drivers (PnpLockdown +
rem  CatalogFile), so we copy the driver file ourselves below.
echo Registering service and settings from INF...
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 "%INSTDIR%\Ext2Fsd.inf"

rem --- put the freshly built driver into System32\drivers --------------------
set "SYS32DRV=%SystemRoot%\System32\drivers\Ext2Fsd.sys"
echo Stopping driver so its file can be replaced...
sc stop Ext2Fsd >nul 2>&1

echo Copying driver to "%SYS32DRV%" ...
copy /y "%INSTDIR%\Ext2Fsd.sys" "%SYS32DRV%" >nul 2>&1
if errorlevel 1 (
    echo   Driver file is in use - scheduling replacement on next reboot...
    set "NEED_REBOOT=1"
    set "SCHEDULE_OK="
    for /f "usebackq delims=" %%r in (`powershell -NoProfile -Command "$s=Join-Path $env:ProgramFiles 'Ext2Fsd\Ext2Fsd.sys'; $d=Join-Path $env:SystemRoot 'System32\drivers\Ext2Fsd.sys'; Add-Type -Namespace W -Name K -MemberDefinition '[System.Runtime.InteropServices.DllImport(\"kernel32.dll\",SetLastError=true,CharSet=System.Runtime.InteropServices.CharSet.Unicode)] public static extern bool MoveFileEx(string a,string b,int f);' -ErrorAction Stop; if([W.K]::MoveFileEx($s,$d,5)){'OK'}else{'FAIL '+[System.Runtime.InteropServices.Marshal]::GetLastWin32Error()}"`) do set "SCHEDULE_OK=%%r"
    if /i "!SCHEDULE_OK!"=="OK" (
        echo   scheduled OK - reboot required to load the new driver.
        set "EXITCODE=1"
    ) else (
        echo   ERROR: could not schedule driver replace: !SCHEDULE_OK!
        echo   Reboot may still be required; on-disk driver was NOT updated.
        set "EXITCODE=1"
        goto :finish
    )
) else (
    echo   Driver file replaced.
    rem Verify staged binary matches System32 copy (size at minimum).
    for %%A in ("%INSTDIR%\Ext2Fsd.sys") do set "STAGED_SIZE=%%~zA"
    for %%A in ("%SYS32DRV%") do set "LIVE_SIZE=%%~zA"
    if not "!STAGED_SIZE!"=="!LIVE_SIZE!" (
        echo   ERROR: System32 driver size !LIVE_SIZE! != staged !STAGED_SIZE!
        set "EXITCODE=1"
        goto :finish
    )
)

rem --- install the helper service --------------------------------------------
if exist "%INSTDIR%\Ext2Srv.exe" (
    echo Installing Ext2Srv service...
    "%INSTDIR%\Ext2Srv.exe" /installasservice
)

rem --- start the driver -------------------------------------------------------
if defined NEED_REBOOT (
    echo.
    echo The running driver could not be replaced live.
    echo A REBOOT is required - the new driver will load automatically on boot.
    set "EXITCODE=1"
) else (
    echo Starting driver...
    net start Ext2Fsd
    if errorlevel 1 (
        echo.
        echo NOTE: the driver did not start.
        if not defined TS_WAS_ON (
            echo   Test-signing was just enabled - REBOOT, then: net start Ext2Fsd
        )
        echo   If Secure Boot is ON, test-signed drivers cannot load: disable
        echo   Secure Boot in firmware, or use a trusted-signed driver.
        set "EXITCODE=1"
    )
)

:finish
echo.
if "!EXITCODE!"=="0" (
    echo === DONE ===
) else (
    echo === DONE WITH WARNINGS/ERRORS ^(exit !EXITCODE!^) ===
)
echo Volume manager: "%INSTDIR%\Ext2Mgr.exe"
if not defined TS_WAS_ON echo A REBOOT is required for test-signing to take effect.
if defined NEED_REBOOT echo *** Please REBOOT to load the new driver. ***
echo.
pause
set "RC=!EXITCODE!"
endlocal & exit /b %RC%

:copyfail
echo ERROR: failed to copy files to "%INSTDIR%".
:fail
echo.
pause
endlocal
exit /b 1
