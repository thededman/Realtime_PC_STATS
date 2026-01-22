# Realtime PC STATS
Using a Lilygo T-Display S3 to update the status of the PC by serial connection.

A tiny, nerdy cyber-tricorder on your desk. :-)

Board used: https://a.co/d/iN3adDc

## Firmware build (PlatformIO)
The Arduino sketch has been migrated to a PlatformIO project.

1. Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI).
2. From the repository root run:
   ```
   pio run -t upload
   ```
   This builds the firmware and flashes it to the LilyGO T-Display S3.
3. Optional: open the serial monitor at 115200 baud.
   ```
   pio device monitor
   ```

Firmware source now lives in `src/main.cpp`. Update your Wi-Fi credentials near the top of that file before building.

### Optional Wi-Fi dashboard
- Wi-Fi is not required for USB stats, but if you want the built-in web view, set `WIFI_SSID` and `WIFI_PASS` near the top of `src/main.cpp`.
- After flashing, the display will show the assigned IP when it connects. Visit `http://<that-ip>/` for the mini dashboard, `/metrics` for JSON, or `/ip` for a plain-text IP echo.
- If the Wi-Fi credentials are left as defaults, the device simply shows `WiFi: not connected` and continues to work over USB.

### Display modes
- The right-side button cycles forward through CPU -> GPU -> Disk views, and the left-side button goes backward.
- Each mode changes the large bar graph, sparkline, and text overlays so you can focus on the stat you care about without re-flashing anything.

## PC feeder requirements
Once the firmware is flashed, run the feeder script to stream stats over USB serial.

### Option 1: Pre-built EXE (recommended)
Download `ESP32_PC_Stats_Feeder.exe` from the releases - no Python installation required.

### Option 2: Run from Python
1. Install dependencies:
   ```
   pip install -r requirements.txt
   ```
2. Run the GUI:
   ```
   python feeder_gui.py
   ```

### Option 3: Build your own EXE
If you want to build the EXE yourself:

1. Make sure Python 3.8+ is installed
2. Run the build script:
   ```
   build_exe.bat
   ```
3. The EXE will be created at `dist\ESP32_PC_Stats_Feeder.exe`

**Optional:** Add an `app.ico` file to the project root for a custom window/tray icon.

## Configuration (secrets)
Copy `include/secrets_template.h` to `include/secrets.h` and fill in your:
- Wi-Fi SSID and password
- OpenWeatherMap API key (free tier works)
- City name for weather

The `secrets.h` file is gitignored to keep your credentials safe.

I am no coder, but I am learning. Most of the code was reviewed by ChatGPT to get the display working, which was a pain.

