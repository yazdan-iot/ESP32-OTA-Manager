    # ESP32-OTA-Manager

Dual-mode OTA update manager for ESP32-S3, combining **ArduinoOTA** (local WiFi push updates) and **HTTP OTA** (periodic pull updates from a server), with automatic **rollback** on failed boots, **FreeRTOS** multitasking, and **NVS**-based update logging.

## ✨ Features

- 🔁 **Two OTA methods**
  - **ArduinoOTA** — push firmware directly from your computer over the local network (password protected).
  - **HTTP OTA** — the device periodically checks a remote/local HTTP server for a new firmware version and downloads it automatically.
- 🛡️ **Automatic rollback** — after every update, the device validates itself (WiFi connectivity + free heap) for 30 seconds. If validation fails, it automatically reverts to the previous working firmware.
- ⚙️ **FreeRTOS multitasking** — 4 independent tasks running in parallel across both cores:
  | Task | Core | Priority | Purpose |
  |---|---|---|---|
  | OTA Task | 1 | 5 | Handles ArduinoOTA |
  | HTTP Check Task | 1 | 1 | Checks for new firmware every 6 hours |
  | Sensor Task | 0 | 2 | Simulated sensor readings (placeholder) |
  | WiFi Monitor Task | 1 | 3 | Reconnects WiFi if dropped |
- 💾 **Persistent logging (NVS)** — boot count, update count, and success/failure stats survive reboots.
- 🧪 **Built-in test server** (`server/server.py`) — a minimal Python HTTP server to simulate an OTA update server for local testing.

## 🧰 Requirements

- ESP32-S3 development board
- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- Python 3 (for the test OTA server)

## 📁 Project Structure

```
ESP32-OTA-Manager/
├── src/
│   └── main.cpp          # Main firmware source
├── server/
│   ├── server.py          # Test OTA HTTP server
│   ├── version.txt        # Current version served (auto-generated)
│   └── firmware.bin        # Compiled firmware to serve (you provide this)
├── platformio.ini          # PlatformIO config (ESP32-S3)
└── README.md
```

## ⚙️ Configuration

Before building, edit these values at the top of `src/main.cpp`:

```cpp
#define FIRMWARE_VERSION    "1.0.0"          // bump this on every release
#define DEVICE_NAME         "ota-manager-01"
#define OTA_PASSWORD         "your-strong-password"
#define WIFI_SSID            "YOUR_SSID"
#define WIFI_PASS            "YOUR_PASSWORD"

#define OTA_SERVER_HOST      "192.168.1.100" // IP of the machine running server.py
#define OTA_SERVER_PORT      8080
```

> ⚠️ **Do not commit real WiFi/OTA passwords to a public repo.** Consider moving secrets to a separate `secrets.h` file and adding it to `.gitignore`.

## 🚀 Build & Flash

```bash
pio run -t upload
pio device monitor
```

## 🌐 Testing HTTP OTA locally

1. Build the project so `firmware.bin` is generated:
   ```bash
   pio run
   ```
2. Copy the compiled binary into the `server/` folder:
   ```bash
   cp .pio/build/esp32-s3-devkitc-1/firmware.bin server/
   ```
3. Bump `FIRMWARE_VERSION` inside `server/server.py` to a newer version than what's currently on the device.
4. Run the test server:
   ```bash
   cd server
   python3 server.py
   ```
5. The device will detect the new version on its next periodic check (or on boot + 1 minute) and update itself automatically.

## 📊 Serial Monitor Output

On boot, the device prints a status report with boot count, update stats, free heap, and WiFi signal strength — useful for debugging OTA behavior over time.

## 📄 License

MIT — feel free to use and modify.