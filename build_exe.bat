@echo off
REM Build ESP32 PC Stats Feeder as standalone EXE
REM Run this from the project root directory

echo Installing/updating dependencies...
pip install -r requirements.txt

echo.
echo Building EXE with PyInstaller...

REM Check if app.ico exists and build accordingly
if exist app.ico (
    echo Found app.ico, including in build...
    pyinstaller --onefile --windowed --name "ESP32_PC_Stats_Feeder" --icon=app.ico --add-data "app.ico;." feeder_gui.py
) else (
    echo No app.ico found, building without custom icon...
    pyinstaller --onefile --windowed --name "ESP32_PC_Stats_Feeder" feeder_gui.py
)

echo.
echo Done! EXE is in: dist\ESP32_PC_Stats_Feeder.exe
pause
