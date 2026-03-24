cmake_minimum_required(VERSION 3.20.0)

# Apply the device-tree overlay that enables I2C20 on P1.2/P1.3
list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/app.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ThermalCam)

target_sources(app PRIVATE
  src/main.c
  src/mlx90640_api.c
  src/mlx90640_i2c.c
  src/ble_stream.c
)
