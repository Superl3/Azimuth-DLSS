@echo off
setlocal
cd /d "%~dp0"
if "%STREAMLINE_SDK%"=="" set STREAMLINE_SDK=%~dp0..\third_party\Streamline
if not exist "%STREAMLINE_SDK%\include\sl_dlss_g.h" exit /b 1
if not exist "%~dp0..\build" mkdir "%~dp0..\build"
cl /nologo /O2 /EHsc /std:c++17 /MT /LD /I"%STREAMLINE_SDK%\include" streamline_live.cpp streamline_vtable_check.cpp ^
  /Fo"%~dp0..\build\\" /Fe"%~dp0..\build\azimuth_streamline_live.dll" ^
  /link d3d12.lib dxgi.lib user32.lib
exit /b %ERRORLEVEL%
