@echo off
setlocal EnableExtensions
set "RYTHMO_CI_SMOKE=%CI%"
set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build\release-windows-arm64"
set "QT_VERSION=6.8.3"
if not defined RYTHMO_VERSION set "RYTHMO_VERSION=0.4.0"
set "QT_ROOT=%QT_ROOT%"
set "VS_HOST=x64"
for /f %%A in ('powershell -NoProfile -Command "[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()" 2^>nul') do if /I "%%A"=="Arm64" set "VS_HOST=arm64"

echo [Rythmo] Build Windows ARM64
where winget >nul 2>nul || if /I not "%CI%"=="true" (echo ERREUR: winget est requis hors CI pour installer automatiquement les dependances manquantes.& exit /b 1)
where git >nul 2>nul || winget install --id Git.Git --exact --accept-package-agreements --accept-source-agreements
where cmake >nul 2>nul || winget install --id Kitware.CMake --exact --accept-package-agreements --accept-source-agreements
where py >nul 2>nul || winget install --id Python.Python.3.12 --exact --accept-package-agreements --accept-source-agreements
call :refresh_path
where git >nul 2>nul || (echo ERREUR: Git requis. Relancez le script apres installation.& exit /b 1)
where cmake >nul 2>nul || (echo ERREUR: CMake requis. Relancez le script apres installation.& exit /b 1)
where py >nul 2>nul || (echo ERREUR: Python requis. Relancez le script apres installation.& exit /b 1)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --includeRecommended" --accept-package-agreements --accept-source-agreements
  if errorlevel 1 exit /b 1
  call :refresh_path
)
call :refresh_path
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT (echo ERREUR: Visual Studio 2022 Build Tools introuvable.& exit /b 1)
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "ARM64_VSROOT=%%V"
if not defined ARM64_VSROOT (
  "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "%VSROOT%" --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --includeRecommended --quiet --norestart
  if errorlevel 1 exit /b 1
)
if not defined QT_ROOT call :find_qt
if not defined QT_ROOT call :install_qt
if not defined QT_ROOT (echo ERREUR: Qt ARM64 6.5+ introuvable.& exit /b 1)
if not exist "%QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" (echo ERREUR: chemin Qt invalide: %QT_ROOT%& exit /b 1)

set "VCPKG_ROOT=%VCPKG_ROOT%"
if not defined VCPKG_ROOT set "VCPKG_ROOT=%ROOT%build\tools\vcpkg"
call :install_vcpkg
if errorlevel 1 exit /b 1
call :vs_env ARM64
if errorlevel 1 exit /b 1
cmake -S "%ROOT%." -B "%BUILD_DIR%" -A ARM64 "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=arm64-windows -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DRYTHMO_TEST_SMOKE="%RYTHMO_CI_SMOKE%" -DRYTHMO_VERSION="%RYTHMO_VERSION%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 exit /b 1
if /I "%CI%"=="true" (
  echo Running Windows unit tests; FFmpeg integration is validated during package preparation and on macOS/Linux CI.
  ctest --test-dir "%BUILD_DIR%" -C Release -R "^rythmo_tests$" --output-on-failure
) else (
  ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
)
if errorlevel 1 exit /b 1
echo.
echo Build termine. Executable: %BUILD_DIR%\Release\Rythmo.exe
exit /b 0

:find_qt
for %%D in ("%USERPROFILE%\Qt\%QT_VERSION%\msvc2022_arm64" "C:\Qt\%QT_VERSION%\msvc2022_arm64") do if exist "%%~D\lib\cmake\Qt6\Qt6Config.cmake" set "QT_ROOT=%%~D"
exit /b 0

:refresh_path
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')" 2^>nul`) do set "PATH=%%P"
exit /b 0

:install_qt
echo Installation de Qt %QT_VERSION% pour Windows ARM64...
py -3 -m pip install --user --upgrade aqtinstall
if errorlevel 1 exit /b 1
py -3 -m aqt install-qt windows_arm64 desktop %QT_VERSION% win64_msvc2022_arm64 --autodesktop --outputdir "%USERPROFILE%\Qt" --modules qtmultimedia
if errorlevel 1 exit /b 1
set "QT_ROOT=%USERPROFILE%\Qt\%QT_VERSION%\msvc2022_arm64"
exit /b 0

:vs_env
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT (echo ERREUR: MSVC ARM64 manquant. Relancez l'installation Build Tools avec la charge C++ ARM64.& exit /b 1)
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=arm64 -host_arch=%VS_HOST%
where cl >nul 2>nul
exit /b %errorlevel%

:install_vcpkg
if not exist "%ROOT%build\tools" mkdir "%ROOT%build\tools"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
if errorlevel 1 exit /b 1
if not exist "%VCPKG_ROOT%\vcpkg.exe" call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 exit /b 1
pushd "%ROOT%" || exit /b 1
"%VCPKG_ROOT%\vcpkg.exe" install --triplet arm64-windows
set "VCPKG_RC=%errorlevel%"
popd
exit /b %VCPKG_RC%
