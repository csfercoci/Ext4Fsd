@echo off
rem ============================================================================
rem  build.cmd - Build the Ext2Fsd / Ext4Fsd driver and user-mode tools.
rem
rem  Builds the driver and user-mode tools with the toolsets installed locally.
rem  Requires Visual Studio 2022/2026 with the WDK (Windows Driver Kit) and the
rem  WindowsKernelModeDriver10.0 platform toolset installed.
rem
rem  Usage:   build.cmd [Configuration] [Platform]
rem           Configuration : Debug (default) | Release
rem           Platform      : x64 (default) | Win32/x86 | ARM | ARM64
rem
rem  Output:  Ext4Fsd\<Configuration>\<Platform>\Ext2Fsd.sys
rem           Ext2Mgr\<Configuration>\<Platform>\Ext2Mgr.exe
rem           Ext2Srv\<Configuration>\<Platform>\Ext2Srv.exe
rem ============================================================================
setlocal EnableExtensions

set "ROOT=%~dp0"
set "MSBUILD_SOLUTION_DIR=%ROOT%\"
set "DRIVER_PROJ=%ROOT%Ext4Fsd\Ext4Fsd.vcxproj"
set "MGR_PROJ=%ROOT%Ext2Mgr\Ext2Mgr.vcxproj"
set "SRV_PROJ=%ROOT%Ext2Srv\Ext2Srv.vcxproj"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "PLATFORM=%~2"
if "%PLATFORM%"=="" set "PLATFORM=x64"

if /i "%CONFIG%"=="debug" set "CONFIG=Debug"
if /i "%CONFIG%"=="release" set "CONFIG=Release"
if /i not "%CONFIG%"=="Debug" if /i not "%CONFIG%"=="Release" (
    echo ERROR: Unknown configuration "%CONFIG%". Use Debug or Release.
    exit /b 1
)

if /i "%PLATFORM%"=="x86" set "PLATFORM=Win32"
if /i "%PLATFORM%"=="win32" set "PLATFORM=Win32"
if /i "%PLATFORM%"=="x64" set "PLATFORM=x64"
if /i "%PLATFORM%"=="arm" set "PLATFORM=ARM"
if /i "%PLATFORM%"=="arm64" set "PLATFORM=ARM64"
if /i not "%PLATFORM%"=="x64" if /i not "%PLATFORM%"=="Win32" if /i not "%PLATFORM%"=="ARM" if /i not "%PLATFORM%"=="ARM64" (
    echo ERROR: Unknown platform "%PLATFORM%". Use x64, Win32/x86, ARM, or ARM64.
    exit /b 1
)

set "OUT_PLATFORM=%PLATFORM%"
if /i "%PLATFORM%"=="Win32" set "OUT_PLATFORM=x86"
if /i "%PLATFORM%"=="ARM" set "OUT_PLATFORM=arm"
if /i "%PLATFORM%"=="ARM64" set "OUT_PLATFORM=arm64"

echo.
echo === Ext4Fsd build : %CONFIG%^|%PLATFORM% ===
echo.

rem --- locate Visual Studio / MSBuild via vswhere -----------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022 or later.
    exit /b 1
)

set "MSBUILD="
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VSINSTALL=%%i"
    if exist "%%i\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%%i\MSBuild\Current\Bin\MSBuild.exe"
)
if not defined MSBUILD (
    echo ERROR: MSBuild.exe not found in any Visual Studio installation.
    exit /b 1
)
echo Using MSBuild: %MSBUILD%

set "USER_TOOLSET="
for /d %%v in ("%VSINSTALL%\MSBuild\Microsoft\VC\v*") do (
    for /d %%t in ("%%~fv\Platforms\%PLATFORM%\PlatformToolsets\v*") do set "USER_TOOLSET=%%~nxt"
)
if not defined USER_TOOLSET (
    echo ERROR: No Visual C++ platform toolset found for %PLATFORM%.
    echo        Install the Visual Studio C++ build tools for this platform.
    exit /b 1
)
echo Using user-mode toolset: %USER_TOOLSET%
echo.

rem --- build ------------------------------------------------------------------
rem  SpectreMitigation=false : Spectre-mitigated libs are an optional component.
rem  ApiValidator_Enable=false : the post-build Universal-driver API check (only
rem    relevant for Store/Universal packaging) fails to launch in some local WDK
rem    installs; it is not needed to produce a loadable driver.
"%MSBUILD%" "%DRIVER_PROJ%" /m /nologo ^
    /p:Configuration=%CONFIG% ^
    /p:Platform=%PLATFORM% ^
    "/p:SolutionDir=%MSBUILD_SOLUTION_DIR%" ^
    /p:SpectreMitigation=false ^
    /p:ApiValidator_Enable=false ^
    /v:minimal

if errorlevel 1 (
    echo.
    echo === BUILD FAILED ===
    exit /b 1
)

rem  Build user-mode tools separately so their VC toolset can track the installed
rem  Visual Studio version without overriding the driver's WDK toolset.
rem  CL=/FS: serialise PDB writes - these old projects compile multiple TUs in
rem  parallel into one .pdb, which newer toolsets reject with C1041 otherwise.
set "CL=/FS"
for %%p in ("%SRV_PROJ%" "%MGR_PROJ%") do (
    "%MSBUILD%" "%%~p" /m /nologo ^
        /p:Configuration=%CONFIG% ^
        /p:Platform=%PLATFORM% ^
        "/p:SolutionDir=%MSBUILD_SOLUTION_DIR%" ^
        /p:PlatformToolset=%USER_TOOLSET% ^
        /p:SpectreMitigation=false ^
        /v:minimal

    if errorlevel 1 (
        echo.
        echo === BUILD FAILED ===
        exit /b 1
    )
)

echo.
echo === BUILD SUCCEEDED ===
echo   Driver : %ROOT%Ext4Fsd\%CONFIG%\%OUT_PLATFORM%\Ext2Fsd.sys
echo   Manager: %ROOT%Ext2Mgr\%CONFIG%\%OUT_PLATFORM%\Ext2Mgr.exe
echo   Service: %ROOT%Ext2Srv\%CONFIG%\%OUT_PLATFORM%\Ext2Srv.exe
echo.
echo Run install.cmd (as Administrator) to install the freshly built driver.
endlocal
exit /b 0
