# NZXT Kraken LCD — OpenRGB Plugin

_OpenRGB plugin for the NZXT Kraken Z / Elite LCD screen._

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20(x64)-0078D6)
![Qt](https://img.shields.io/badge/Qt-5.15-41CD52)
![OpenRGB](https://img.shields.io/badge/OpenRGB-plugin-CC1010)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C)
![License](https://img.shields.io/badge/license-MIT-3DA639)

Drive the circular LCD of **NZXT Kraken Z / Elite** coolers straight from [OpenRGB](https://openrgb.org) **without NZXT CAM**.

> **Windows-only.** The screen channel relies on a WinUSB transport and the sensor backend on a .NET assembly. There is no Linux/macOS build for now.

---

## Features

### Display modes

| Mode | Description |
|------|-------------|
| **Image / GIF** | Show any still image or animated GIF (JPG / PNG / GIF) as a full-screen background. |
| **Single Infographic** | One sensor value with a gradient gauge arc. Accepts an optional animated GIF background. |
| **Dual Infographic** | Two sensors side by side (e.g. CPU + GPU) displayed as vertical arcs or horizontal bars, with optional GIF background. |
| **Triple Infographic** | One primary sensor with two smaller secondary values — useful for combining temperature, load and clock at a glance. Optional GIF background. |
| **Clockface** | A live clock in three styles: analog dots, gradient arc, or large digital readout. |
| **Audio Visualizer** | Real-time frequency bars driven by WASAPI loopback capture (system audio). Sensitivity is adjustable. |
| **Now Playing** | Pulls the current track title, artist, album art and playback progress from Windows SMTC — works with Spotify, browsers and any SMTC-aware app. |

### Available sensors

CPU temperature · CPU load · CPU clock · GPU temperature · GPU load · RAM load · Coolant (liquid) temperature · Pump RPM · Fan RPM

### General

- Per-mode color and gradient customisation, with an independent logo toggle.
- °C / °F follows the OpenRGB locale setting.
- Brightness and rotation controls.
- Refresh rate is driven by the panel: ~5 Hz for static screens, full panel rate (30 or 60 Hz) for animated modes.

---

## Supported devices

| Model | PID | Resolution | Refresh |
|-------|-----|-----------|---------|
| NZXT Kraken Z53 | `0x3009` | 480×480 | 30 Hz |
| NZXT Kraken Z63 | `0x300A` | 480×480 | 30 Hz |
| NZXT Kraken Z73 | `0x3008` | 480×480 | 30 Hz |
| NZXT Kraken 2023 | `0x300E` | 480×480 | 30 Hz |
| NZXT Kraken Elite 360 | `0x300C` | 480×480 | 60 Hz |
| NZXT Kraken Elite V2 (2024) | `0x3012` | 640×640 | 60 Hz |
| NZXT Kraken Elite V2 (alt) | `0x3013` | 640×640 | 60 Hz |

All devices share VID `0x1E71`. Primary development target: **Kraken Elite V2 (`0x3012`)**.

---

## Installation

### 1. Get the plugin

Download the prebuilt **`NZXTKrakenLCDPlugin.dll`** from the [Releases](https://github.com/LeProta/NZXTKrakenLCDPlugin/releases) page, or [build it yourself](#building-from-source).

### 2. Place `lhwm-wrapper.dll` next to `OpenRGB.exe`

System sensors are read through [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) via this bridge DLL.

**[Download](https://gitlab.com/OpenRGBDevelopers/OpenRGBHardwareSyncPlugin/-/raw/master/dependencies/lhwm-cpp-wrapper/x64/Release/lhwm-wrapper.dll) `lhwm-wrapper.dll`** (from the OpenRGB Hardware Sync plugin) and place it in the **same folder as `OpenRGB.exe`**. Without it, the plugin loads but shows *"Cannot load the plugin"*.

### 3. Install the plugin

In OpenRGB: **Settings → Plugins → Install plugin** → select `NZXTKrakenLCDPlugin.dll`.

Or drop it directly into:
```
%APPDATA%\OpenRGB\plugins
```

Restart OpenRGB — a **NZXT Kraken LCD** tab will appear.

### 4. Run OpenRGB as Administrator

Required to read CPU and motherboard temperatures (ring-0 sensor driver). Liquid temperature, pump/fan RPM and GPU sensors work without it.

---

## Building from source

### Requirements

| Tool | Version |
|------|---------|
| Visual Studio Build Tools | 2019 / 2022 — *Desktop development with C++* + *.NET Framework 4.x SDK* |
| Qt | 5.15.2 `msvc2019_64` |
| vcpkg | latest — provides `hidapi` and `libusb` (`x64-windows`) |
| CMake | ≥ 3.16 |
| OpenRGB source | 0.9+ |

### Steps

Use an **x64 Native Tools Command Prompt for VS**:

```bat
git clone https://github.com/LeProtagoniste/NZXTKrakenLCDPlugin
cd NZXTKrakenLCDPlugin

cmake -S . -B build -G "NMake Makefiles" ^
  -DOPENRGB_INCLUDE_DIR=C:/OpenRGB-source ^
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64

cmake --build build
```

Output: `build/NZXTKrakenLCDPlugin.dll`.

> If `hidapi` / `libusb` are not found automatically, pass the vcpkg toolchain:
> `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`

> The build is forced to Release — `lhwm-cpp-wrapper.lib` is compiled with `/MD` and mixing runtimes causes `LNK2038`.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| No **NZXT Kraken LCD** tab / *"Cannot load the plugin"* | `lhwm-wrapper.dll` is missing from the OpenRGB folder. |
| CPU temperature/clock shows 0 / N/A | Close NZXT CAM / HWiNFO / Ryzen Master; run OpenRGB as Administrator; or check that *Memory Integrity (HVCI)* / the *Vulnerable Driver Blocklist* is not blocking the sensor driver. |
| GPU temperature/clock/pourcentage shows 0 / N/A | Make sure you have correctly installed your graphics drivers. |

Logs are written next to OpenRGB's own log files (`NZXTKrakenLCD_<timestamp>.log`), and make sure you have enabled logs in the OpenRGB settings for the logs to appear.

---

## How it works

- **Two USB channels.** Interface 0 is the vendor bulk endpoint (`0x02` OUT) used to stream LCD frames via libusb/WinUSB. Interface 1 is HID, used for control commands, liquid temperature and pump/fan RPM.
- **Frame pipeline.** Render with `QPainter` → encode (Q565 for the 640×640 Elite V2, JPEG for 480×480 Z-series) → HID handshake (`0x36 0x01` / wait for `0x37 0x01`) → bulk transfer → `0x36 0x02` end. Sends run on a dedicated worker thread.
- **Sensors.** System metrics from LibreHardwareMonitor (`lhwm-wrapper.dll`); coolant temperature and RPMs from the cooler over HID.
- **Cadence.** Animated modes run at the panel maximum (30 or 60 Hz); static stat screens update at ~5 Hz.

---

## Acknowledgements

- [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) — host application and plugin SDK.
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) — system sensor backend (MPL-2.0).
- `lhwm-cpp-wrapper` — C++/CLI wrapper shipped with the [OpenRGB Hardware Sync plugin](https://gitlab.com/OpenRGBDevelopers/OpenRGBHardwareSyncPlugin).

---

## License

Released under the **MIT License** — see [`LICENSE`](LICENSE).

Third-party components (LibreHardwareMonitor, Qt, hidapi, libusb) remain under their respective licenses.
