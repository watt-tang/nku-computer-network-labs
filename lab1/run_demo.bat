@echo off
setlocal
cd /d "%~dp0"

if not exist "bin\server.exe" (
  echo [ERROR] Executables are missing. Run build.bat first.
  pause
  exit /b 1
)
if not exist "bin\client.exe" (
  echo [ERROR] Executables are missing. Run build.bat first.
  pause
  exit /b 1
)

start "NEXUS CHAT Server" "%~dp0bin\server.exe" 8888
timeout /t 2 /nobreak >nul
start "NEXUS CHAT Alice" "%~dp0bin\client.exe" 127.0.0.1 8888 Alice
start "NEXUS CHAT Bob" "%~dp0bin\client.exe" 127.0.0.1 8888 Bob

echo Demo windows launched. Start a third client manually if needed:
echo "%~dp0bin\client.exe" 127.0.0.1 8888 Charlie
