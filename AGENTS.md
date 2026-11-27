# AGENTS.md

## Project

Windows desktop app: movable crosshair overlay rendered via Direct2D transparent layered windows, paired with a unified macOS 26 (Tahoe) Liquid Glass native Win32/GDI+ UI suite (custom tray popup menu, preferences panel, About card, live-progress Update window, and modal glass alerts). C++20, Win32 API only.

## Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

- **MSVC-only** — `CMakeLists.txt` enforces `FATAL_ERROR` for non-MSVC compilers.
- **Ninja generator** — pass `-G Ninja` with cmake.
- Build output is written to the **project root** (`${PROJECT_SOURCE_DIR}`).
- Release links `/MT` (static runtime); Debug uses `/MDd` (dynamic).
- Executable output name is strictly `CrossPointer.exe`.

## Architecture

| File(s)                      | Role                                                                                                                                                                     |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `main.cpp`                   | `WinMain` entry — single-instance mutex, DPI awareness, immersive dark mode sync, `TaskbarCreated` recovery, tray click disambiguation, window orchestration             |
| `src/version.h`              | Centralized semantic version constants (`1.0` / `v1.0`)                                                                                                                  |
| `src/liquid_glass.cpp/h`     | Ref-counted GDI+ session, macOS 26 Liquid Glass palette, DWM chrome, squircle icon, traffic-light close, pill buttons, status orbs, progress bar, and modal `ShowAlert`  |
| `src/tray_menu.cpp/h`        | Frameless macOS 26 Liquid Glass custom tray context menu with inset status header, vector glyphs, pill hover, keyboard navigation, and work-area clamping                |
| `src/about_window.cpp/h`     | Frameless macOS 26 Liquid Glass About card with hero squircle icon, version badge, feature card, and clickable GitHub link                                               |
| `src/update_window.cpp/h`    | Frameless macOS 26 Liquid Glass Software Update window with animated checking bar, live streaming download progress + speed, cancel button, and silent background check  |
| `src/settings_window.cpp/h`  | macOS 26 Liquid Glass double-buffered GDI+ settings panel: custom toggle switches, capsule pills, custom sliders, keycap toggles, live crosshair canvas, toast           |
| `src/crosshair.cpp/h`        | Four transparent `WS_EX_LAYERED` + `WS_EX_TRANSPARENT` topmost popup windows. Zero-idle-wakeup 1ms/240Hz tracking, zero-VSync-stall Direct2D `Clear` rendering, DPI cache |
| `src/tray.cpp/h`             | Native Win32 system tray icon manager (`Shell_NotifyIconW`), Explorer restart recovery, DPI-aware icon loading, dynamic tooltip                                          |
| `src/config.cpp/h`           | Atomic INI save (`.tmp` + `MoveFileExW`), cache flush, system theme detection (`RegGetValueW`), light/dark & fixed color mapping, full OEM hotkey parsing/serialization  |
| `src/hotkey.cpp/h`           | Registers/unregisters global hotkeys via `RegisterHotKey` with single-key reuse support                                                                                  |
| `src/autostart.cpp/h`        | Portable HKCU Run-key autostart management                                                                                                                               |
| `src/updater.cpp/h`          | Semantic-version GitHub release checker, HTTP 200 + `Content-Length` streaming downloader with speed/cancel callback, PE MZ check, atomic self-update with graceful exit |
| `src/config_file_util.cpp/h` | Shared long-path `GetExecutablePath()`, embedded `kDefaultIni`, strict pure-green `CrossPointer.ini` next to EXE                                                         |
| `res/resource.rc`            | App icon (macOS 26 squircle aesthetic) and `VFT_APP` version info                                                                                                        |
