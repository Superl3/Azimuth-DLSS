@echo off
setlocal
powershell.exe -NoProfile -File "%~dp0Start-DebugCapture.ps1" %*
if errorlevel 1 echo Debug capture did not complete successfully.
pause
