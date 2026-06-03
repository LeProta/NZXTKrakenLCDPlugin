# NZXT Kraken LCD — OpenRGB Plugin

_OpenRGB plugin for the NZXT Kraken Z / Elite LCD screen._

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20(x64)-0078D6)
![Qt](https://img.shields.io/badge/Qt-5.15-41CD52)
![OpenRGB](https://img.shields.io/badge/OpenRGB-plugin-CC1010)
![C++](https://img.shields.io/badge/C%2B%2B-17-00599C)
![License](https://img.shields.io/badge/license-MIT-3DA639)

Drive the circular LCD of **NZXT Kraken Z / Elite** coolers straight from
[OpenRGB](https://openrgb.org) — system stats, clock faces, an audio
visualizer, a *playing* widget and custom image/GIF backgrounds — **without
NZXT CAM**.

> **Windows-only.** The screen channel relies on a WinUSB transport and the
> sensor backend on a .NET assembly; there is no Linux/macOS build.

<!-- Add screenshots / GIFs of the modes here, e.g. ![Single](docs/single.png) -->

---

## Table of contents

- [Features](#features)
- [Supported devices](#supported-devices)
- [Download](#download)
- [Requirements](#requirements)
- [Setup (required to make it work)](#setup-required-to-make-it-work)
- [Building from source](#building-from-source)
- [Usage](#usage)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
- [Acknowledgements](#acknowledgements)
- [License](#license)

---

## Features

| Mode | Description |
|------|-------------|
| **Image / GIF** | Custom still image or animated GIF (JPG / PNG / GIF) |
| **Single Infographic** | One sensor with a gradient gauge arc (optional GIF background) |
| **Dual Infographic** | Two sensors (e.g. CPU + GPU), vertical arcs or horizontal bars (optional GIF) |
| **Triple Infographic** | One primary + two secondary sensors (optional GIF) |
| **Clockface** | Clock with three styles (analog dots, gradient arc, digital) |
| **Audio Visual** | Real-time audio visualizer (WASAPI loopback) with adjustable sensitivity |
| **Now Playing** | Media title / artist / cover / progress via Windows SMTC (Spotify, browsers, …) |

Additional capabilities:

- **Sensors** — CPU/GPU temperature, load and clock; RAM load; coolant (liquid)
  temperature; pump & fan RPM.
- **Per-mode configuration** — each mode keeps its own colors, gradients, logo
  toggle and background.
- **°C / °F** — follows the OpenRGB locale, applied to the displayed value only.
- **Brightness & rotation** control.
- Refresh rate driven by each model's panel (30 Hz on Z-series, 60 Hz on Elite);
  animated modes run at the panel's maximum.

## Supported devices

| Model | VID | PID | Resolution | Max refresh |
|-------|-----|-----|-----------|-------------|
| NZXT Kraken Z53 | `0x1E71` | `0x3009` | 480×480 | 30 Hz |
| NZXT Kraken Z63 | `0x1E71` | `0x300A` | 480×480 | 30 Hz |
| NZXT Kraken Z73 | `0x1E71` | `0x3008` | 480×480 | 30 Hz |
| NZXT Kraken 2023 | `0x1E71` | `0x300E` | 480×480 | 30 Hz |
| NZXT Kraken Elite 360 | `0x1E71` | `0x300C` | 480×480 | 60 Hz |
| NZXT Kraken Elite V2 (2024) | `0x1E71` | `0x3012` | 640×640 | 60 Hz |
| NZXT Kraken Elite V2 (alt) | `0x1E71` | `0x3013` | 640×640 | 60 Hz |

> Primary development and testing target: **Kraken Elite V2 (`0x3012`)**. Older
> models share the same protocol but are less extensively tested.

## Download

Prebuilt **`NZXTKrakenLCDPlugin.dll`** binaries are published on the
[Releases](https://github.com/LeProtagoniste/NZXTKrakenLCDPlugin/releases) page
for convenience. They are compiled directly from the source in this repository —
and because the full source is available, you can [build it yourself](#building-from-source)
and verify the result rather than trusting the binary. The only external binary is
`lhwm-wrapper.dll`, taken as-is from the official
[OpenRGB Hardware Sync plugin](https://gitlab.com/OpenRGBDevelopers/OpenRGBHardwareSyncPlugin).

Either way, complete the [required setup](#setup-required-to-make-it-work)
(Zadig + `lhwm-wrapper.dll`) before first use.

## Requirements

### Runtime

| Component | Why it is needed | Where to get it |
|-----------|------------------|-----------------|
| **OpenRGB** | Host application that loads the plugin | <https://openrgb.org> · [GitLab](https://gitlab.com/CalcProgrammer1/OpenRGB) |
| **WinUSB driver** on the Kraken LCD interface | Lets the plugin claim the bulk LCD channel | Installed with [Zadig](https://zadig.akeo.ie) (see [Setup](#setup-required-to-make-it-work)) |
| **`lhwm-wrapper.dll`** next to `OpenRGB.exe` | .NET bridge to LibreHardwareMonitor (system sensors) | [Direct download](https://gitlab.com/OpenRGBDevelopers/OpenRGBHardwareSyncPlugin/-/raw/master/dependencies/lhwm-cpp-wrapper/x64/Release/lhwm-wrapper.dll) |
| **Administrator rights** for OpenRGB | Required to read CPU / motherboard temperatures (ring-0 sensor driver) | — |

### Build

| Tool | Version | Notes |
|------|---------|-------|
| [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) | 2019 / 2022 | "Desktop development with C++" **and** the **.NET Framework 4.x SDK** component (provides `mscoree.lib`, required by the C++/CLI sensor wrapper) |
| [Qt](https://download.qt.io/archive/qt/5.15/5.15.2/) | 5.15.2 (`msvc2019_64`) | Core, Widgets, Concurrent |
| [vcpkg](https://github.com/microsoft/vcpkg) | latest | provides `hidapi` and `libusb` (`x64-windows`) |
| [CMake](https://cmake.org/download/) | ≥ 3.16 | NMake generator used here |
| OpenRGB source tree | 0.9+ | for the plugin API headers |

## Setup (required to make it work)

Three things are **mandatory**, in addition to copying the plugin:

### 1. Install the WinUSB driver on the LCD interface (Zadig)

The LCD frames travel over a vendor bulk endpoint that Windows does not expose
to user space by default. [Zadig](https://zadig.akeo.ie) installs a WinUSB
driver on it:

1. Launch **Zadig**.
2. **Options → List All Devices**.
3. In the dropdown, select **`NZXT Kraken … (Interface 0)`**.
4. Pick **WinUSB** as the target driver and click **Replace Driver**.

> ⚠️ This takes the LCD channel away from **NZXT CAM**. Use one controller at a
> time. Interface 0 = LCD bulk; leave Interface 1 (HID, used for sensors and
> control commands) untouched.

### 2. Place `lhwm-wrapper.dll` next to `OpenRGB.exe`

System sensors (CPU/GPU/RAM) are read through an embedded build of
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor).
Download **[`lhwm-wrapper.dll`](https://gitlab.com/OpenRGBDevelopers/OpenRGBHardwareSyncPlugin/-/raw/master/dependencies/lhwm-cpp-wrapper/x64/Release/lhwm-wrapper.dll)**
(the same assembly shipped with the OpenRGB *Hardware Sync* plugin) and place it
in the **same folder as `OpenRGB.exe`**. If it is missing, the plugin loads but
its panel is disabled with a *"Cannot load the plugin"* message.

### 3. Run OpenRGB as Administrator

CPU and motherboard temperatures require a ring-0 driver loaded by
LibreHardwareMonitor — only available when OpenRGB runs **as Administrator**.
Liquid temperature, pump/fan RPM and GPU sensors work without it.

> **Recommended:** disable the Kraken's RGB driver in `OpenRGB.json` so OpenRGB
> does not contend with this plugin over the device's HID channel.

### 4. Install the plugin

In OpenRGB, open **Settings → Plugins**, click **Install plugin** and select
`NZXTKrakenLCDPlugin.dll`. Alternatively, drop the file straight into the plugins folder:

```
%APPDATA%\OpenRGB\plugins
```

Restart OpenRGB — a **"NZXT Kraken LCD"** tab then appears.

## Building from source

Use an **x64 Native Tools Command Prompt for VS**.

```bat
git clone https://github.com/LeProtagoniste/NZXTKrakenLCDPlugin
cd NZXTKrakenLCDPlugin

:: Configure (NMake generator). Adjust the paths to your environment.
cmake -S . -B build -G "NMake Makefiles" ^
  -DOPENRGB_INCLUDE_DIR=C:/OpenRGB-source ^
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64

:: Build
cmake --build build
```

Output: `build/NZXTKrakenLCDPlugin.dll`.

> **vcpkg paths.** `CMakeLists.txt` currently contains fallback paths pointing at
> a local vcpkg install (`C:/Users/<user>/vcpkg/...`). If `hidapi` / `libusb` are
> not found automatically, either edit those fallbacks or pass the toolchain:
> `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`.

> **Release only.** The bundled `lhwm-cpp-wrapper.lib` is built against the `/MD`
> (Release) CRT, so the build is forced to Release to avoid `LNK2038` mismatches.
> Re-run the `cmake -S . -B build …` configure step only when you add/remove
> source files; otherwise `cmake --build build` is enough.

## Usage

1. Open OpenRGB and go to the **NZXT Kraken LCD** tab.
2. Pick a **display mode** and assign sensors / colors / background as desired.
3. Adjust **brightness** and **rotation** if needed.
4. The live preview mirrors what is sent to the screen.

Settings are persisted by OpenRGB and restored on the next launch.

## Troubleshooting

| Symptom | Likely cause & fix |
|---------|--------------------|
| No **NZXT Kraken LCD** tab, or *"Cannot load the plugin"* | `lhwm-wrapper.dll` is not next to `OpenRGB.exe`. |
| Screen not detected / log says *bulk USB unavailable* | WinUSB driver not installed on **Interface 0** — run Zadig (step 1). |
| **CPU temperature/clock shows 0 / N/A** | Another app holds the CPU sensor bus — close **NZXT CAM**, Armoury Crate, Ryzen Master, HWiNFO; **or** OpenRGB is not running as Administrator; **or** *Memory Integrity (HVCI)* / the *Vulnerable Driver Blocklist* is blocking the sensor driver. |
| GPU values empty | Make sure GPU sensors appear in the plugin log; some require admin too. |
| LCD **flickering** | Usually a transport/handshake issue — confirm WinUSB on Interface 0 and that **only one** app controls the LCD (close CAM). |
| Build error `LNK2038` / `mscoree.lib` not found | Install the **.NET Framework 4.x SDK** component in the Visual Studio Installer, then re-run CMake. |

Logs are written next to OpenRGB's log files
(`NZXTKrakenLCD_<timestamp>.log`) and are useful when reporting issues.

## How it works

- **Two USB channels.** *Interface 0* is the vendor bulk channel (endpoint
  `0x02` OUT) used to stream LCD frames, claimed through libusb/WinUSB.
  *Interface 1* is HID, used for control commands and reading liquid temperature
  and pump/fan RPM.
- **Frame pipeline.** Render with `QPainter` → encode (**Q565** for the 640×640
  Elite V2, **JPEG** for the 480×480 Z-series) → HID start handshake
  (`0x36 0x01` → wait for the `0x37 0x01` ack) → bulk header + payload →
  `0x36 0x02` end. Sends run on a dedicated worker thread.
- **Sensors.** System metrics come from an embedded LibreHardwareMonitor
  (`lhwm-wrapper.dll`) with automatic mapping by hardware identifier; coolant
  temperature and RPMs come from the cooler over HID.
- **Cadence.** Each model advertises a maximum refresh (30 Hz Z-series, 60 Hz
  Elite); animated modes render at that maximum, static stat screens at ~5 Hz.

## Acknowledgements

- [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) — host application and plugin SDK.
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) — system sensor backend (MPL-2.0).
- `lhwm-cpp-wrapper` — the C++/CLI wrapper over LibreHardwareMonitor that produces `lhwm-wrapper.dll`. A prebuilt copy ships with the [OpenRGB Hardware Sync plugin](https://gitlab.com/OpenRGBDevelopers/OpenRGBHardwareSyncPlugin).
- [liquidctl](https://github.com/liquidctl/liquidctl) and the wider community for Kraken protocol research.
- [Zadig](https://zadig.akeo.ie) — WinUSB driver installation.

## License

Released under the **MIT License** — see [`LICENSE`](LICENSE).

Third-party components (LibreHardwareMonitor, Qt, hidapi, libusb, …) remain under
their respective licenses.
