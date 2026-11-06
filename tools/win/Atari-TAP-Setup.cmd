@echo off
setlocal
set "ATARI_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "ATARI_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%ATARI_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tap-setup.ps1"
endlocal
