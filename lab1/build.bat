@echo off
setlocal
cd /d "%~dp0"

if not exist "bin" mkdir "bin"

where cl >nul 2>nul
if %errorlevel%==0 goto :build_msvc

where g++ >nul 2>nul
if %errorlevel%==0 goto :build_mingw

echo [ERROR] No supported C++ compiler was found.
echo.
echo Option 1: Open "x64 Native Tools Command Prompt for Visual Studio" and run build.bat.
echo Option 2: Install MinGW-w64 and add g++.exe to PATH, then run build.bat.
exit /b 1

:build_msvc
echo [INFO] Building with MSVC...
cl /nologo /std:c++17 /EHsc /utf-8 /W4 server.cpp /Fe:bin\server.exe ws2_32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /utf-8 /W4 client.cpp /Fe:bin\client.exe ws2_32.lib
if errorlevel 1 exit /b 1
goto :success

:build_mingw
echo [INFO] Building with MinGW-w64...
g++ -std=c++17 -O2 -Wall -Wextra -static-libgcc -static-libstdc++ server.cpp -o bin\server.exe -lws2_32
if errorlevel 1 exit /b 1
g++ -std=c++17 -O2 -Wall -Wextra -static-libgcc -static-libstdc++ client.cpp -o bin\client.exe -lws2_32
if errorlevel 1 exit /b 1
goto :success

:success
echo.
echo [SUCCESS] Build completed.
echo Executables:
echo   bin\server.exe
echo   bin\client.exe
echo.
echo Run: bin\server.exe 8888
echo Run: bin\client.exe 127.0.0.1 8888 Alice
exit /b 0
