# ESP32-S3 Fluid Watch Face

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4.3-blue.svg)](https://github.com/espressif/esp-idf)
[![Chip](https://img.shields.io/badge/Chip-ESP32--S3-orange.svg)](https://www.espressif.com/en/products/socs/esp32s3)
[![Platform](https://img.shields.io/badge/Platform-LVGL-red.svg)](https://lvgl.io/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

A smartwatch face application for ESP32-S3 featuring a **real-time SPH fluid simulation** that responds to accelerometer tilt input. Displays analog clock hands, digital time/date, and renders fluid particles on an LVGL canvas.

![ESP32-S3-EYE](https://img.shields.io/badge/Hardware-ESP32--S3--EYE-blue)

## ✨ Features

- **Analog Clock Display** - Classic watch face with hour, minute, and second hands
- **Digital Time & Date** - Shows current time and date with weekday
- **Real-time Fluid Simulation** - 42-particle SPH (Smoothed Particle Hydrodynamics) fluid that flows based on device tilt
- **Multi-Sensor Support** - Auto-detects accelerometers:
  - QMA7981 (primary)
  - MPU6050
  - LIS3DH / SC7A20
  - Button fallback for testing without sensor
- **Input Processing Pipeline** - Deadzone filtering, tilt limiting, curve adjustment, and low-pass filtering for smooth fluid response

## 🛠️ Tech Stack

| Component | Technology |
|-----------|------------|
| **MCU** | ESP32-S3 |
| **Framework** | ESP-IDF v5.4.3 |
| **UI Framework** | LVGL |
| **Display** | ESP32-S3-EYE LCD (via BSP) |
| **Sensor** | Accelerometer (I2C) |
| **Connectivity** | WiFi + SNTP for time sync |

## 📦 Prerequisites

- ESP-IDF v5.4.3 or later
- ESP32-S3-EYE development board
- USB-C cable for flashing
- (Optional) Working accelerometer on I2C bus

## 🚀 Quick Start

### 1. Environment Setup

```bash
# Activate ESP-IDF environment
D:\Espressif\frameworks\esp-idf-v5.4.3\export.bat
```

### 2. Build the Project

```bash
# Set target chip (first time only)
idf.py set-target esp32s3

# Build
idf.py build
```

### 3. Flash and Monitor

```bash
# Flash to device and open serial monitor
idf.py -p COM7 flash monitor
```

### 4. Configure WiFi

Edit `main/main.c` to set your WiFi credentials:

```c
#define WIFI_SSID "your_ssid"
#define WIFI_PASS "your_password"
```

## 📁 Project Structure

```
watch_face2/
├── main/
│   ├── main.c           # Entry point, WiFi/SNTP init, timers, input processing
│   ├── watch_ui.c       # LVGL UI: clock face, hands, digital display, fluid canvas
│   ├── fluid_sim.c      # SPH fluid simulation (density/pressure/forces/rendering)
│   ├── fluid_sim.h      # Fluid simulation API header
│   ├── accel_input.c    # IMU sensor abstraction with auto-detection
│   ├── accel_input.h    # Accelerometer API header
│   └── CMakeLists.txt   # Component build configuration
├── managed_components/  # ESP-IDF managed components (LVGL, BSP, etc.)
├── CMakeLists.txt       # Project build configuration
├── sdkconfig            # ESP-IDF configuration
├── CLAUDE.md            # Development notes
└── README.md            # This file
```

## 🧠 Architecture

### Data Flow

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  accel_input.c  │ ──▶ │     main.c      │ ──▶ │   watch_ui.c    │ ──▶ │   fluid_sim.c   │
│  IMU polling    │     │  Input filter   │     │  LVGL canvas    │     │  SPH physics    │
│  Auto-detect    │     │  30Hz timer     │     │  Render layer   │     │  Particle draw  │
└─────────────────┘     └─────────────────┘     └─────────────────┘     └─────────────────┘
```

### Input Pipeline

```
Raw Accelerometer Data
    │
    ▼
Bias Calibration (first sample)
    │
    ▼
Deadzone (0.03g ≈ 1.7°)
    │
    ▼
Tilt Limit (0.40g ≈ 23°)
    │
    ▼
Input Curve (linear, power=1.0)
    │
    ▼
Low-pass Filter (α=0.80)
    │
    ▼
Fluid Simulation Input
```

### Fluid Simulation

The SPH fluid uses:
- **Poly6 kernel** for density estimation
- **Spiky gradient** for pressure forces
- **Viscous Laplacian** for viscosity
- **Spatial hashing** (16px grid cells) for neighbor lookup
- **XSPH velocity smoothing** for particle stability

Key parameters (tunable in `fluid_sim.c`):
- Particle count: 42
- Stiffness: 2300
- Viscosity: 3.5
- Gravity scale: 1800

## 📋 Configuration

### Fluid Input Tuning (`main/main.c`)

```c
#define FLUID_INPUT_FILTER_ALPHA  0.80f   // Low-pass filter coefficient
#define FLUID_INPUT_DEADZONE_G    0.03f   // Deadzone threshold (~1.7°)
#define FLUID_INPUT_MAX_TILT_G    0.40f   // Max tilt input (~23°)
#define FLUID_INPUT_CURVE_POWER   1.0f    // Input curve (1.0 = linear)
```

### Fluid Physics (`main/fluid_sim.c`)

```c
#define FLUID_PARTICLE_COUNT      42      // Number of fluid particles
#define FLUID_STIFFNESS           2300.0f // Pressure stiffness
#define FLUID_VISCOSITY           3.5f    // Dynamic viscosity
#define FLUID_GRAVITY_SCALE       1800.0f // Gravity multiplier
```

## 📷 Screenshots

*Add screenshots here when available*

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- [ESP-IDF](https://github.com/espressif/esp-idf) by Espressif Systems
- [LVGL](https://github.com/lvgl/lvgl) - Light and Versatile Graphics Library
- [ESP32-S3-EYE BSP](https://github.com/espressif/esp-bsp) by Espressif

## 📧 Contact

For questions or issues, please open an issue on GitHub.
