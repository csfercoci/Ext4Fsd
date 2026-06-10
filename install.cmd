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
rem           Configuration : Release (default) | Debug
rem           Platform      : x64 (default) | Win32 | ARM64
rem
rem  Run build.cmd first to produce the binaries.
rem ============================================================================
setlocal EnableExtensions

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
if "%CONFIG%"=="" set "CONFIG=Release"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"

set "DRV=%ROOT%Ext4Fsd\%CONFIG%\%PLATFORM%\Ext2Fsd.sys"
set "INF=%ROOT%Ext4Fsd\Ext2Fsd.inf"
set "CER=%ROOT%Setup\Ext2Fsd.cer"
set "MGR=%ROOT%Ext2Mgr\%CONFIG%\%PLATFORM%\Ext2Mgr.exe"
set "SRV=%ROOT%Ext2Srv\%CONFIG%\%PLATFORM%\Ext2Srv.exe"
set "INSTDIR=%ProgramFiles%\Ext2Fsd"

echo.
echo === Ext2Fsd install : %CONFIG%^|%PLATFORM% ===
echo.

if not exist "%DRV%" (
    echo ERROR: driver not found:
    echo   %DRV%
    echo Run build.cmd first.
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
set "NEED_REBOOT="
echo Stopping driver so its file can be replaced...
sc stop Ext2Fsd >nul 2>&1

echo Copying driver to "%SYS32DRV%" ...
copy /y "%INSTDIR%\Ext2Fsd.sys" "%SYS32DRV%" >nul 2>&1
if errorlevel 1 (
    echo   Driver file is in use - scheduling replacement on next reboot...
    powershell -NoProfile -Command "Add-Type -Namespace W -Name K -MemberDefinition '[System.Runtime.InteropServices.DllImport(\"kernel32.dll\",SetLastError=true,CharSet=System.Runtime.InteropServices.CharSet.Unicode)] public static extern bool MoveFileEx(string a,string b,int f);'; $s=Join-Path $env:ProgramFiles 'Ext2Fsd\Ext2Fsd.sys'; $d=Join-Path $env:SystemRoot 'System32\drivers\Ext2Fsd.sys'; if([W.K]::MoveFileEx($s,$d,5)){'  scheduled OK'}else{'  schedule FAILED err='+[System.Runtime.InteropServices.Marshal]::GetLastWin32Error()}"
    set "NEED_REBOOT=1"
) else (
    echo   Driver file replaced.
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
    )
)

echo.
echo === DONE ===
echo Volume manager: "%INSTDIR%\Ext2Mgr.exe"
if not defined TS_WAS_ON echo A REBOOT is required for test-signing to take effect.
if defined NEED_REBOOT echo *** Please REBOOT to load the new driver. ***
echo.
pause
endlocal
exit /b 0

:copyfail
echo ERROR: failed to copy files to "%INSTDIR%".
:fail
echo.
pause
endlocal
exit /b 1
