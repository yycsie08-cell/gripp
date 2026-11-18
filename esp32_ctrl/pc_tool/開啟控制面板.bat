@echo off
rem Open the GameHand flashing panel. Try normal Python first, then the one bundled with PlatformIO.
cd /d "%~dp0"
where pythonw >nul 2>nul && (start "" pythonw panel.py & exit /b)
where py >nul 2>nul && (start "" py panel.py & exit /b)
if exist "%USERPROFILE%\.platformio\penv\Scripts\python.exe" ("%USERPROFILE%\.platformio\penv\Scripts\python.exe" panel.py & exit /b)
python panel.py
if errorlevel 1 pause
