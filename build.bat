@echo off
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
for /f %%A in ('powershell -NoProfile -Command "[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()" 2^>nul') do set "NATIVE_ARCH=%%A"
if /I "%NATIVE_ARCH%"=="Arm64" (
  call "%SCRIPT_DIR%build-windows-arm64.bat" %*
) else (
  call "%SCRIPT_DIR%build-windows-x64.bat" %*
)
exit /b %errorlevel%
