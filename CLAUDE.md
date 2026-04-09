# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 smartwatch face application featuring a **fluid simulation** that responds to accelerometer tilt input. The watch displays analog clock hands, digital time/date, and a real-time SPH (Smoothed Particle Hydrodynamics) fluid rendered on an LVGL canvas.

**Target Hardware:** ESP32-S3-EYE development board  
**ESP-IDF Version:** 5.4.3  
**Display:** LCD via ESP32-S3-EYE BSP  
**Input:** Accelerometer (auto-detects QMA7981, MPU6050, LIS3DH, SC7A20) with button fallback

## Build & Flash Commands

```cmd
D:\Espressif\frameworks\esp-idf-v5.4.3\export.bat
idf.py build
idf.py -p COM7 flash monitor
```

## Code Architecture

### Module Structure

```
main/
├── main.c           # Entry point, WiFi/SNTP init, timer loops, accel input processing
├── watch_ui.c       # LVGL UI: clock face, hands, digital time, fluid canvas container
├── fluid_sim.c      # SPH fluid simulation (42 particles, spatial hashing grid)
├── accel_input.c    # IMU sensor abstraction with auto-detection and button fallback
```

### Data Flow

1. **`accel_input.c`** polls accelerometer → applies deadzone/curve/filter → outputs `x_g`, `y_g` tilt values
2. **`main.c`** (`update_fluid_timer_cb` @ 30Hz) processes raw accel data and calls `watch_ui_update_fluid()`
3. **`watch_ui.c`** passes tilt to `fluid_sim_step()` and triggers `fluid_sim_render()` on LVGL canvas
4. **`fluid_sim.c`** runs SPH physics (density/pressure/forces) and renders particles to ARGB8888 buffer

### Key Components

- **Fluid Simulation**: Position-based dynamics with spatial hashing (grid cell = 16px). Uses Poly6 kernel for density, Spiky gradient for pressure, viscous Laplacian for viscosity.
- **Input Pipeline**: Raw accel → bias calibration → deadzone (0.03g) → limit (0.40g) → curve (linear) → low-pass filter (α=0.80)
- **LVGL Integration**: `LV_DRAW_BUF_DEFINE_STATIC` for fluid canvas draw buffer; rendering uses `lv_canvas_init_layer()` / `lv_draw_rect()` / `lv_canvas_finish_layer()`

### Sensor Support

Accelerometer auto-detection order:
1. QMA7981 (I2C addresses 0x12-0x19, auto-detects data format)
2. MPU6050 (0x68/0x69)
3. LIS3DH/SC7A20 (0x18/0x19)
4. Button fallback (BSP_BUTTON_1-4 as tilt simulation)

### Configuration Constants

All tunable parameters are `#define`d at the top of each module:
- `fluid_sim.c`: particle count (42), stiffness (2300), viscosity (3.5), gravity scale (1800)
- `main.c`: filter alpha (0.80), deadzone (0.03g), max tilt (0.40g)
- `watch_ui.c`: canvas size (132x132), color schemes, layout offsets
