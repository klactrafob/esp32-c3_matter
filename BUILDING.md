# Build Guide (ESP-IDF)

This project is built with ESP-IDF (`idf.py`), not PlatformIO.

## Prerequisites

- ESP-IDF `v5.5.x`
- Python environment created by ESP-IDF tools
- `CMake 3.28` or `3.29` (CMake `3.30+` is currently not compatible with ESP-IDF Component Manager in this project)

## Windows quick start

1. Open **ESP-IDF PowerShell** (or run your `export.ps1`).
2. Ensure `cmake --version` is `3.28.x` or `3.29.x`.
3. Build:

```powershell
idf.py reconfigure
idf.py build
```

If your shell resolves `cmake` to `3.30+`, prepend a 3.29 install to `PATH` before running `idf.py`.

## Notes

- `main/CMakeLists.txt` explicitly requires `esp_timer` because sources include `esp_timer.h`.
- Do not commit machine-specific VS Code settings (local paths to ESP-IDF, Python, CMake, toolchains).
