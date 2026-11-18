@echo off
rem Call PlatformIO pio without needing PATH. Used by global VS Code shortcuts (Ctrl+Alt+U / M / B).
set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if exist "%PIO%" goto run
set "PIO=pio"
:run
"%PIO%" %*
exit /b %ERRORLEVEL%
