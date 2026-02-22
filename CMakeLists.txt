cmake_minimum_required(VERSION 3.16)

# CMake 3.30+ breaks ESP-IDF Component Manager (define_property not scriptable)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.30")
  message(FATAL_ERROR
    "CMake 3.30+ is not compatible with ESP-IDF Component Manager.\n"
    "Use CMake 3.28 or 3.29. In VS Code: ESP-IDF settings -> Cmake path -> set to a CMake 3.28/3.29 install.")
endif()

# Ensure project directory is scanned for components (main/ must be here)
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_c3_matter_web)

# Apply to whole project (override with -DAPP_ENABLE_MATTER=1 if needed)
add_compile_definitions(APP_ENABLE_MATTER=0)
