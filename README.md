# Realtime PC STATS

A real-time PC monitoring dashboard on an M5Stack Core3, featuring touch screen navigation and weather display.

**Board:** [M5Stack Core3](https://shop.m5stack.com/products/m5stack-cores3-esp32s3-lotdevelopment-kit) (ESP32-S3, 320x240 touch display)

## Features

- **PC Stats**: CPU, Memory, GPU, Disk usage with animated bar graphs and sparklines
- **Weather**: Current conditions and 3-day forecast via OpenWeatherMap
- **Touch Navigation**: Swipe left/right to change display modes
- **WiFi Dashboard**: Access stats from any browser on your network
- **Captive Portal Setup**: No code editing required - configure WiFi and weather via web browser

## Quick Start

### 1. Flash the Firmware

Install [PlatformIO](https://platformio.org/install) (VS Code extension or CLI), then:

```bash
pio run -t upload
```

### 2. First-Time Setup (Captive Portal)

On first boot (or after erasing settings), the device enters setup mode:

1. Connect your phone/laptop to WiFi network: **`PCMonitor-Setup`**
2. A captive portal should open automatically (or go to `http://172.16.1.1`)
3. Enter your:
   - WiFi network name and password
   - OpenWeatherMap API key ([get free key here](https://openweathermap.org/api))
   - City name (e.g., "New York", "London")
   - Temperature units (Fahrenheit or Celsius)
4. Click **Save & Connect** - device will reboot and connect to your WiFi

### 3. Run the PC Feeder

The feeder app sends your PC stats to the display over USB serial.

**Option A: Pre-built EXE** (recommended)
Download `ESP32_PC_Stats_Feeder.exe` from releases - no Python needed.

**Option B: Run from Python**
```bash
pip install -r requirements.txt
python feeder_gui.py
```

**Option C: Build your own EXE**
```bash
build_exe.bat
```

## Display Modes

Swipe on the touch screen to navigate:

| Mode | Shows |
|------|-------|
| **CPU** | CPU usage %, memory %, CPU temp, sparkline |
| **GPU** | GPU usage %, GPU temp, sparkline |
| **Disk** | Disk usage %, throughput (MB/s), free space |
| **Weather** | Current temp, conditions, 3-day forecast |

## Re-entering Setup Mode

To change WiFi or weather settings after initial setup:

**Long-press the screen for 3 seconds** - the device will enter setup mode and start the captive portal.

Alternatively, erase all settings and start fresh:
```bash
pio run -t erase && pio run -t upload
```

## WiFi Web Dashboard

Once connected to WiFi, the device shows its IP address on screen. Access the web dashboard:

| Endpoint | Description |
|----------|-------------|
| `http://<ip>/` | Live HTML dashboard with all stats |
| `http://<ip>/metrics` | JSON API for all data |
| `http://<ip>/ip` | Plain text IP address |

The web dashboard includes:
- Real-time PC stats (CPU, GPU, Memory, Disk)
- Weather with forecast
- Data freshness indicator (shows if feeder is connected)

## Feeder GUI Options

- **Serial Port**: Select the COM port for your M5Stack
- **GPU Selection**: Choose which GPU to monitor (multi-GPU support)
- **Disk Scale**: Set what MB/s = 100% on the bar graph
- **Drives**: Choose which drives to report free space for
- **Minimize to Tray**: Run in background with system tray icon

## CPU Temperature on Windows

CPU temperature reading on Windows requires one of:
- Running the feeder as **Administrator** (for WMI access)
- Installing the optional `wmi` Python package: `pip install wmi`

GPU temperature works reliably via NVIDIA drivers (NVML or nvidia-smi).

## Troubleshooting

**"No serial data received yet" on web dashboard**
- Make sure the feeder GUI is running and connected to the correct COM port
- Check that the M5Stack is connected via USB

**Weather shows "--" or wrong temperature**
- Verify your OpenWeatherMap API key is valid
- Check city name spelling
- Long-press to re-enter setup and verify settings

**Can't connect to captive portal**
- Make sure you're connected to the `PCMonitor-Setup` WiFi network
- Try manually navigating to `http://172.16.1.1`
- Some devices may need to disable mobile data temporarily

## Project Structure

```
├── src/
│   ├── main.cpp           # Main firmware (display, web server, touch)
│   ├── config_portal.cpp  # Captive portal for WiFi/weather setup
│   ├── weather_api.cpp    # OpenWeatherMap API client
│   └── weather_display.cpp # Weather screen rendering
├── include/
│   ├── config_portal.h
│   ├── weather_config.h
│   └── Free_Fonts.h
├── feeder_gui.py          # PC stats feeder with GUI
├── platformio.ini         # PlatformIO build config
└── requirements.txt       # Python dependencies
```

## Credits

Built with M5Unified library for M5Stack Core3. Weather data from OpenWeatherMap.
