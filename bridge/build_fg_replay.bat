@echo off
setlocal
cd /d "%~dp0"
if "%DLSS_SDK%"=="" set DLSS_SDK=%~dp0..\third_party\DLSS
set LIBDIR=%DLSS_SDK%\lib\Windows_x86_64\x64
if not exist "%DLSS_SDK%\include\nvsdk_ngx_helpers_dlssg_d3d.h" exit /b 1
if not exist "%LIBDIR%\nvsdk_ngx_s.lib" exit /b 1
if not exist "%~dp0..\build" mkdir "%~dp0..\build"
cl /nologo /O2 /EHsc /std:c++17 /MT /I"%DLSS_SDK%\include" fg_replay.cpp ^
   /Fo"%~dp0..\build\\" /Fe"%~dp0..\build\azimuth_fg_replay.exe" ^
   /link /LIBPATH:"%LIBDIR%" nvsdk_ngx_s.lib d3d12.lib dxgi.lib user32.lib advapi32.lib
exit /b %ERRORLEVEL%
