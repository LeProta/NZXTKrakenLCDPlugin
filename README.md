# NZXT Kraken LCD plugin for OpenRGB

Drives the round LCD screen of NZXT Kraken Z and Elite coolers from OpenRGB, without NZXT CAM: images and GIFs, sensor infographics, a clock, an audio visualizer and a "now playing" screen.

[![Release](https://img.shields.io/github/v/release/LeProta/NZXTKrakenLCDPlugin)](https://github.com/LeProta/NZXTKrakenLCDPlugin/releases)
![OpenRGB](https://img.shields.io/badge/OpenRGB-1.1-blue)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-lightgrey)
[![License](https://img.shields.io/github/license/LeProta/NZXTKrakenLCDPlugin)](LICENSE)

Requires OpenRGB 1.1. If you are still on OpenRGB 1.0rc3, use [v1.0.6](https://github.com/LeProta/NZXTKrakenLCDPlugin/releases/tag/v1.0.6).

## Features

Screen modes:

- Image or GIF: any JPG, PNG or GIF as a full screen background.
- Single infographic: one sensor on a gradient arc, with an optional GIF behind it.
- Dual infographic: two sensors side by side (CPU and GPU, for example), as vertical arcs or horizontal bars.
- Triple infographic: one main sensor and two smaller ones, for example temperature, load and clock.
- Clock: analog dots, a gradient arc, or a big digital clock.
- Audio visualizer: frequency bars from what your PC is playing (WASAPI loopback), with adjustable sensitivity.
- Now playing: title, artist, cover art and progress of the current track, read from the Windows media controls. Works with Spotify, browsers and anything else that reports to Windows.

Sensors: CPU temperature, load and clock, GPU temperature and load, RAM load, coolant temperature, pump RPM and fan RPM.

Each mode has its own colors and gradients, and the logo can be turned on or off per mode. Brightness and rotation are adjustable. Temperatures are shown in °C or °F depending on the OpenRGB language setting.

GIFs and the visualizer run at the full rate of the panel (30 or 60 Hz), the clock and "now playing" at 30 fps, and the other screens at 5 fps. A frame that has not changed is sent only once a second, so a still image costs almost nothing over USB.

The ring LEDs stay under OpenRGB's control. OpenRGB only sends their colors once at startup, and a packet lost while the cooler boots can leave an LED dark, so the plugin asks OpenRGB to resend its own colors every 2 seconds. This only happens in per-LED (Direct) mode, so hardware effects are not restarted.

## Supported coolers

| Model | Resolution | Refresh |
|-------|-----------|---------|
| Kraken Z53 (`0x3009`) | 480×480 | 30 Hz |
| Kraken Z63 (`0x300A`) | 480×480 | 30 Hz |
| Kraken Z73 (`0x3008`) | 480×480 | 30 Hz |
| Kraken 2023 (`0x300E`) | 480×480 | 30 Hz |
| Kraken Elite 360 (`0x300C`) | 480×480 | 60 Hz |
| Kraken Elite V2 (`0x3012`, `0x3013`) | 640×640 | 60 Hz |

All of them use vendor ID `0x1E71`. Development and testing are done on a Kraken Elite V2.

## Installation

1. Download `NZXTKrakenLCDPlugin.dll` from the [releases page](https://github.com/LeProta/NZXTKrakenLCDPlugin/releases).
2. Download `lhwm-wrapper.dll` from [LeProta/lhwm-wrapper](https://github.com/LeProta/lhwm-wrapper/releases) and put it next to `OpenRGB.exe`. The plugin reads the system sensors through it. The [pump plugin](https://github.com/LeProta/NZXTKrakenPumpPlugin) uses the same file, one copy is enough for both.
3. Install [PawnIO](https://pawnio.eu) 2.1.0 or newer. LibreHardwareMonitor needs this driver to read the CPU and motherboard sensors.
4. In OpenRGB, go to Settings > Plugins > Install plugin and pick the DLL, or copy it to `%APPDATA%\OpenRGB\plugins`. Restart OpenRGB and a "NZXT Kraken LCD" tab shows up.
5. Run OpenRGB as administrator to get CPU and motherboard temperatures. Coolant temperature, pump and fan RPM and the GPU sensors work without it.

Nothing else is needed: hidapi and libusb come with OpenRGB 1.1.

Keep only plugin files in the plugins folder. OpenRGB tries to load everything in there, backups included.

## Troubleshooting

| Problem | What to try |
|---------|-------------|
| No "NZXT Kraken LCD" tab | Check that you are on OpenRGB 1.1. Older versions need [v1.0.6](https://github.com/LeProta/NZXTKrakenLCDPlugin/releases/tag/v1.0.6). |
| The tab only says "Cannot load the plugin" | `lhwm-wrapper.dll` is missing from the OpenRGB folder. |
| CPU temperature or clock shows 0 or N/A | Close NZXT CAM, HWiNFO and Ryzen Master, and run OpenRGB as administrator. Check that PawnIO is installed and that Windows is not blocking it (Memory Integrity, vulnerable driver blocklist). |
| GPU temperature, clock or load shows 0 or N/A | Reinstall or update the graphics driver. |

Logs are written to `%APPDATA%\OpenRGB\logs\NZXTKrakenLCD_<date>.log`. They follow OpenRGB's own log settings, and the last 10 files are kept by default.

## Building from source

You need:

- Visual Studio Build Tools 2022 with the C++ desktop workload
- Qt 6.8.3 (`msvc2022_64`)
- CMake 3.16 or newer (the one that comes with Visual Studio works)
- libusb, for example `vcpkg install libusb:x64-windows`
- [lhwm-wrapper](https://github.com/LeProta/lhwm-wrapper) cloned next to this repository

The rest is in `third_party/`: the OpenRGB 1.1 plugin API headers, and an import library for the `hidapi-hotplug.dll` that ships with OpenRGB.

From an x64 Native Tools Command Prompt, in a short path (NMake cannot open files whose full path is longer than 260 characters):

```bat
git clone https://github.com/LeProta/lhwm-wrapper
git clone https://github.com/LeProta/NZXTKrakenLCDPlugin
cd NZXTKrakenLCDPlugin

cmake -S . -B build -G "NMake Makefiles" ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake

cmake --build build
```

The result is `build/NZXTKrakenLCDPlugin.dll`.

Some notes:

- Without vcpkg, pass `-DLIBUSB_ROOT=<folder>` instead (a folder with `include/libusb-1.0/libusb.h` and `lib/libusb-1.0.lib`).
- `-DLHWM_WRAPPER_DIR=<folder>` points to another copy of the wrapper.
- The build is always Release: OpenRGB refuses plugins linked to the debug Qt DLLs, and `lhwm-cpp-wrapper.lib` is built with `/MD`.
- `/EHa` is required so that `catch (...)` also catches the .NET exceptions thrown by the wrapper.
- If OpenRGB changes its hidapi exports, regenerate the import library with `lib /def:hidapi.def /machine:x64 /out:hidapi.lib` in `third_party/hidapi/lib`.

### Benchmark

`tests/bench_render` measures the render and encoding time of each mode, without any hardware:

```bat
cmake -S tests -B build-bench -G "NMake Makefiles" -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-bench
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%
build-bench\bench_render.exe 200
```

## How it works

- The cooler has two USB interfaces. Interface 0 is a vendor bulk endpoint (`0x02` OUT) that takes the screen frames, through libusb and WinUSB. Interface 1 is HID, used for commands, coolant temperature and pump and fan RPM.
- A frame is drawn with `QPainter`, encoded (Q565 on the 640×640 Elite V2, JPEG on the 480×480 models), announced over HID with `0x36 0x01`, sent over bulk once the cooler answers `0x37 0x01`, and closed with `0x36 0x02`. All of this runs on a worker thread.
- System sensors come from LibreHardwareMonitor through `lhwm-wrapper.dll`. Coolant temperature and RPM come from the cooler over HID.
- OpenRGB settings (config folder, language, hex format, logs) are read through the plugin API and refreshed when they change.
- The LCD plugin, the [pump plugin](https://github.com/LeProta/NZXTKrakenPumpPlugin) and OpenRGB's own Kraken driver can all use the cooler at the same time. Each opens its own HID handle and Windows shares the device.

## Credits

- [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) for the plugin API
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) for the sensors (MPL-2.0)
- [lhwm-wrapper](https://github.com/LeProta/lhwm-wrapper), the bridge between this plugin and LibreHardwareMonitor
- [PawnIO](https://pawnio.eu) for the hardware access driver

See also the [pump plugin](https://github.com/LeProta/NZXTKrakenPumpPlugin), which controls the pump and fans of the same coolers.

## License

MIT, see [LICENSE](LICENSE). Qt, hidapi, libusb and LibreHardwareMonitor keep their own licenses. The OpenRGB plugin API headers in `third_party/openrgb` are GPL-2.0-or-later (see the `LICENSE` file there), and `nlohmann/json.hpp` is MIT.
