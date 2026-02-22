cmake_minimum_required(VERSION 3.16)

# Ensure project directory is scanned for components (main/ must be here)
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_c3_matter_web)
