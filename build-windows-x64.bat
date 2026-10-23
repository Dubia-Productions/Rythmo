@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "BUILD_DIR=%ROOT%build\release-windows-x64"
set "DIST_DIR=%ROOT%dist\Rythmo-by-Dubia-Productions-windows-x64-v8"
set "QT_ARCH=win64_msvc2022_64"
set "QT_HOST=windows"
set "QT_VERSION=6.8.3"
if not defined RYTHMO_VERSION set "RYTHMO_VERSION=0.3.0"

echo [Rythmo] Build Windows x64
where winget >nul 2>nul || if /I not "%CI%"=="true" (echo ERREUR: winget est requis hors CI pour installer automatiquement les dependances manquantes.& exit /b 1)
where git >nul 2>nul || call :install "Git.Git"
where cmake >nul 2>nul || call :install "Kitware.CMake"
where py >nul 2>nul || call :install "Python.Python.3.12"
call :refresh_path
where git >nul 2>nul || (echo ERREUR: Git requis. Relancez le script apres installation.& exit /b 1)
where cmake >nul 2>nul || (echo ERREUR: CMake requis. Relancez le script apres installation.& exit /b 1)
where py >nul 2>nul || (echo ERREUR: Python requis pour preparer Qt. Relancez le script apres installation.& exit /b 1)

call :ensure_vs
if errorlevel 1 exit /b 1

set "QT_ROOT=%QT_ROOT%"
if not defined QT_ROOT call :find_qt
if not defined QT_ROOT call :install_qt
if not defined QT_ROOT (echo ERREUR: Qt 6.5+ introuvable apres installation.& exit /b 1)
if not exist "%QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" (echo ERREUR: Qt_ROOT ne contient pas lib\cmake\Qt6\Qt6Config.cmake: %QT_ROOT%& exit /b 1)

set "VCPKG_ROOT=%VCPKG_ROOT%"
if not defined VCPKG_ROOT set "VCPKG_ROOT=%ROOT%build\tools\vcpkg"
call :install_vcpkg
if errorlevel 1 exit /b 1

call :vs_env x64
if errorlevel 1 exit /b 1
cmake -S "%ROOT%" -B "%BUILD_DIR%" -A x64 "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DRYTHMO_VERSION="%RYTHMO_VERSION%" -DRYTHMO_DISTRIBUTION_DIRECTORY="%DIST_DIR%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 exit /b 1
ctest --test-dir "%BUILD_DIR%" -C Release --output-on-failure
if errorlevel 1 exit /b 1
echo.
echo Build termine. Executable: %DIST_DIR%\Rythmo.exe
echo Tests reussis. Le package Windows x64 a ete mis a jour dans dist.
exit /b 0

:install
echo Installation de %~1...
winget install --id %~1 --exact --accept-package-agreements --accept-source-agreements
set "INSTALL_RC=%errorlevel%"
call :refresh_path
exit /b %INSTALL_RC%

:refresh_path
for /f "usebackq delims=" %%P in (`powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')" 2^>nul`) do set "PATH=%%P"
exit /b 0

:ensure_vs
echo Recherche des outils C++ Visual Studio 2022...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended" --accept-package-agreements --accept-source-agreements
  if errorlevel 1 exit /b 1
  call :refresh_path
)
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT (echo ERREUR: Visual Studio 2022 Build Tools introuvable.& exit /b 1)
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCTOOLS_ROOT=%%V"
if not defined VCTOOLS_ROOT (
  "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "%VSROOT%" --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet --norestart
  if errorlevel 1 exit /b 1
)
call :vs_env x64
exit /b %errorlevel%

:vs_env
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo ERREUR: Visual Studio C++ Build Tools introuvable.& exit /b 1)
for /f "usebackq delims=" %%V in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT (echo ERREUR: charge de travail C++ MSVC non installee.& exit /b 1)
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=%~1 -host_arch=x64
where cl >nul 2>nul
exit /b %errorlevel%

:find_qt
for %%D in ("%USERPROFILE%\Qt\%QT_VERSION%\msvc2022_64" "C:\Qt\%QT_VERSION%\msvc2022_64" "%USERPROFILE%\Qt\6.8.3\msvc2022_64" "C:\Qt") do if exist "%%~D\lib\cmake\Qt6\Qt6Config.cmake" set "QT_ROOT=%%~D"
exit /b 0

:install_qt
echo Installation de Qt %QT_VERSION% pour MSVC x64 (Widgets, Multimedia)...
py -3 -m pip install --user --upgrade aqtinstall
if errorlevel 1 exit /b 1
py -3 -m aqt install-qt %QT_HOST% desktop %QT_VERSION% %QT_ARCH% --outputdir "%USERPROFILE%\Qt" --modules qtmultimedia
if errorlevel 1 exit /b 1
set "QT_ROOT=%USERPROFILE%\Qt\%QT_VERSION%\msvc2022_64"
exit /b 0

:install_vcpkg
where git >nul 2>nul || (echo ERREUR: git est requis pour Expat/vcpkg.& exit /b 1)
if not exist "%ROOT%build\tools" mkdir "%ROOT%build\tools"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
if errorlevel 1 exit /b 1
if not exist "%VCPKG_ROOT%\vcpkg.exe" call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 exit /b 1
pushd "%ROOT%" || exit /b 1
"%VCPKG_ROOT%\vcpkg.exe" install --triplet x64-windows
set "VCPKG_RC=%errorlevel%"
popd
exit /b %VCPKG_RC%
