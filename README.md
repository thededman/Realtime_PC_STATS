# Realtime PC STATS
Using a Lilygo T-Display S3 to update the status of the PC by serial connection.

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

## PC feeder requirements
Once the firmware is flashed, run the feeder script to stream stats over USB serial.

- Update COM port values inside `feeder.py` (or use `Feeder.exe`).
- Install feeder dependencies:
  ```
  python -m pip install psutil pyserial GPUtil
  ```

I have also included a `Feeder.exe`, which makes this easier if you do not want to install Python.

I am no coder, but I am learning. Most of the code was reviewed by ChatGPT to get the display working, which was a pain.

