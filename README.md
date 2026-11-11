# NZXT Kraken LCD plugin for OpenRGB

An OpenRGB plugin that drives the round LCD screen of NZXT Kraken Z and Elite coolers. It works without NZXT CAM.

Windows only (10 and 11, 64-bit).

## What the screen can show

- Image or GIF: any JPG, PNG or GIF as a full screen background.
- Single infographic: one sensor on a gradient arc. A GIF can be used as the background.
- Dual infographic: two sensors side by side (CPU and GPU, for example), as vertical arcs or horizontal bars. GIF background is optional.
- Triple infographic: one main sensor and two smaller ones, for example temperature, load and clock. GIF background is optional.
- Clock: analog dots, a gradient arc, or a big digital clock.
- Audio visualizer: frequency bars from what your PC is playing (WASAPI loopback). The sensitivity can be adjusted.
- Now playing: title, artist, cover art and progress of the current track, read from Windows media controls. It works with Spotify, browsers and anything else that reports to Windows.

Available sensors: CPU temperature, load and clock, GPU temperature and load, RAM load, coolant temperature, pump RPM and fan RPM.

Each mode has its own colors and gradients, and you can turn the logo on or off separately. You can also set brightness and rotation. Temperatures are shown in °C or °F depending on the OpenRGB language setting.

The screen refreshes about 5 times a second for static screens. Animated modes use the full rate of the panel (30 or 60 Hz).

## Supported coolers

| Model | Resolution | Refresh |
|-------|-----------|---------|
| Kraken Z53 (`0x3009`) | 480×480 | 30 Hz |
| Kraken Z63 (`0x300A`) | 480×480 | 30 Hz |
| Kraken Z73 (`0x3008`) | 480×480 | 30 Hz |
| Kraken 2023 (`0x300E`) | 480×480 | 30 Hz |
| Kraken Elite 360 (`0x300C`) | 480×480 | 60 Hz |
| Kraken Elite V2 (`0x3012`, `0x3013`) | 640×640 | 60 Hz |

All of them use vendor ID `0x1E71`. The plugin is developed and tested on a Kraken Elite V2.

## Install

1. Download `NZXTKrakenLCDPlugin.dll` from the [releases page](https://github.com/LeProta/NZXTKrakenLCDPlugin/releases).
2. Download `lhwm-wrapper.dll` from [LeProta/lhwm-wrapper](https://github.com/LeProta/lhwm-wrapper/releases) and put it next to `OpenRGB.exe`. The plugin reads the system sensors through it. The [pump plugin](https://github.com/LeProta/NZXTKrakenPumpPlugin) uses the same file, so one copy is enough for both.
3. Install [PawnIO](https://pawnio.eu) (2.1.0 or newer). LibreHardwareMonitor needs this driver to read CPU and motherboard temperatures.
4. In OpenRGB go to Settings, Plugins, Install plugin and pick the DLL. You can also copy it to `%APPDATA%\OpenRGB\plugins`. Restart OpenRGB and a "NZXT Kraken LCD" tab shows up.
5. Run OpenRGB as administrator to get CPU and motherboard temperatures. Coolant temperature, pump and fan RPM and the GPU sensors work without it.

Without `lhwm-wrapper.dll` the plugin loads but only shows "Cannot load the plugin".

Keep only plugin files in the plugins folder. OpenRGB tries to load everything in there, backups included.

## Building it yourself

You need:

- Visual Studio Build Tools 2019 or 2022 with the C++ desktop workload
- Qt 6.8.3 (`msvc2022_64`), for OpenRGB 1.1 (plugin API 5)
- CMake 3.16 or newer (the one that comes with Visual Studio is fine)
- hidapi and libusb: the bundled `third_party/hidapi/lib/hidapi.lib` imports OpenRGB 1.1's own `hidapi-hotplug.dll` (regenerate with `lib /def:hidapi.def /machine:x64`); libusb from `third_party/libusb` or vcpkg
- [lhwm-wrapper](https://github.com/LeProta/lhwm-wrapper) cloned next to this repository

The OpenRGB plugin interface is already in `src/NZXTKrakenLCDPlugin.h`, so you do not need the OpenRGB sources.

Open an x64 Native Tools Command Prompt and use a short path, because NMake cannot open files whose full path is longer than 260 characters.

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

If you do not use vcpkg, pass `-DHIDAPI_ROOT=<folder> -DLIBUSB_ROOT=<folder>` instead of the toolchain file. `-DLHWM_WRAPPER_DIR=<folder>` points to a different copy of the wrapper. The build is always Release, because OpenRGB refuses plugins linked to the debug Qt DLLs and `lhwm-cpp-wrapper.lib` is built with `/MD`. `/EHa` is needed so `catch (...)` can catch the .NET exceptions coming from the wrapper.

## Problems

| What you see | What to try |
|--------------|-------------|
| No "NZXT Kraken LCD" tab, or "Cannot load the plugin" | `lhwm-wrapper.dll` is missing from the OpenRGB folder. |
| CPU temperature or clock shows 0 or N/A | Close NZXT CAM, HWiNFO and Ryzen Master, and run OpenRGB as administrator. Also check that PawnIO is installed and that Windows (Memory Integrity, the vulnerable driver blocklist) is not blocking the driver. |
| GPU temperature, clock or load shows 0 or N/A | Check that your graphics driver is installed properly. |
| OpenRGB crashes a few seconds after start with `0xC0000005` in `*_unloaded` | Update to 1.0.4 or newer. The graphics driver was unloading one of its DLLs while a sensor was being read. |

Logs go next to the OpenRGB logs, in `NZXTKrakenLCD_<timestamp>.log`. You have to turn logging on in the OpenRGB settings to get them.

## Notes for developers

- The cooler has two USB interfaces. Interface 0 is a vendor bulk endpoint (`0x02` OUT) that takes the screen frames, through libusb and WinUSB. Interface 1 is HID and is used for commands, coolant temperature and pump and fan RPM.
- A frame is drawn with `QPainter`, encoded (Q565 on the 640×640 Elite V2, JPEG on the 480×480 Z series), announced over HID with `0x36 0x01`, sent over bulk once the cooler answers `0x37 0x01`, and closed with `0x36 0x02`. Sending runs on its own worker thread.
- System sensors come from LibreHardwareMonitor through `lhwm-wrapper.dll`. Coolant temperature and RPM come from the cooler over HID.
- The LCD plugin and the [pump plugin](https://github.com/LeProta/NZXTKrakenPumpPlugin) can run on the same cooler. Each one opens its own HID handle and Windows takes care of sharing it.

## Credits

- [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) for the plugin interface
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) for the sensors (MPL-2.0)
- [lhwm-wrapper](https://github.com/LeProta/lhwm-wrapper), the bridge between this plugin and LibreHardwareMonitor
- [PawnIO](https://pawnio.eu) for the hardware access driver

The [pump plugin](https://github.com/LeProta/NZXTKrakenPumpPlugin) controls the pump and fans of the same coolers.

## License

MIT, see [LICENSE](LICENSE). Qt, hidapi, libusb and LibreHardwareMonitor keep their own licenses. The OpenRGB 1.1 plugin API headers in `third_party/openrgb` are GPL-2.0-or-later (see `third_party/openrgb/LICENSE`); `nlohmann/json.hpp` is MIT.
