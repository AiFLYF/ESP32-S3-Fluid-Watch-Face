# ESP32-S3 流体表盘

[English README](./README_EN.md)

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.4.3-blue.svg)](https://github.com/espressif/esp-idf)
[![Chip](https://img.shields.io/badge/Chip-ESP32--S3-orange.svg)](https://www.espressif.com/en/products/socs/esp32s3)
[![UI](https://img.shields.io/badge/UI-LVGL-red.svg)](https://lvgl.io/)
[![Board](https://img.shields.io/badge/Board-ESP32--S3--EYE-1f6feb.svg)](https://github.com/espressif/esp-bsp)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](./LICENSE)

一个运行在 **ESP32-S3-EYE** 上的智能手表表盘项目，集成了 **模拟时钟 + 数字时间日期 + 基于 SPH 的实时流体模拟**。设备倾斜时，流体会随加速度输入产生动态响应，适合用作嵌入式图形界面、传感器交互和流体效果演示。

## 项目简介与价值主张

这个项目基于 **ESP-IDF 5.4.3** 和 **LVGL** 构建，目标是用较小的硬件成本实现一个具有视觉表现力的表盘应用：

- 不只是显示时间，还能通过流体效果增强交互感
- 支持多种常见加速度传感器自动识别
- 没有 IMU 时也能通过按键回退进行调试
- 适合作为 ESP32-S3 图形项目、传感器联动项目的参考实现

## ✨ 核心功能

- **模拟时钟显示**：时针、分针、秒针实时刷新
- **数字时间与日期**：显示年月日、星期等信息
- **实时流体模拟**：42 个粒子的 SPH 流体在 LVGL Canvas 上渲染
- **倾斜交互**：设备姿态变化会实时驱动流体流动
- **多传感器兼容**：自动识别以下加速度传感器
  - QMA7981
  - MPU6050
  - LIS3DH / SC7A20
- **按键回退输入**：无传感器时可用板载按键模拟倾斜输入
- **联网校时**：通过 WiFi + SNTP 自动同步时间

## 🛠️ 技术栈

| 类别 | 技术 |
|---|---|
| 主控 | ESP32-S3 |
| 开发框架 | ESP-IDF 5.4.3 |
| 图形界面 | LVGL |
| 开发板 | ESP32-S3-EYE |
| 显示 | BSP LCD 驱动 |
| 输入 | I2C 加速度计 / 板载按键 |
| 网络 | WiFi / SNTP |
| 语言 | C |

## 📦 安装步骤

### 1. 准备环境

请先安装并配置好 ESP-IDF 5.4.3。

```bat
D:\Espressif\frameworks\esp-idf-v5.4.3\export.bat
```

### 2. 编译项目

首次编译建议先设置目标芯片：

```bat
idf.py set-target esp32s3
idf.py build
```

### 3. 烧录并打开串口监视器

```bat
idf.py -p COM7 flash monitor
```

## 🚀 快速开始 / 使用示例

### 配置 WiFi

项目当前在 `main/main.c` 中写死了 WiFi 配置，烧录前请先修改：

```c
#define WIFI_SSID "your_ssid"
#define WIFI_PASS "your_password"
```

### 运行方式

烧录成功后：

1. 启动设备并进入表盘界面
2. 连接 WiFi 并进行 SNTP 校时
3. 倾斜开发板，观察流体响应
4. 如果没有检测到 IMU，可使用按键模拟方向输入

## 📁 项目目录结构

```text
watch_face2/
├── main/
│   ├── main.c           # 程序入口、WiFi/SNTP 初始化、定时器与输入处理
│   ├── watch_ui.c       # LVGL 表盘界面、时间显示、流体画布容器
│   ├── watch_ui.h
│   ├── fluid_sim.c      # SPH 流体模拟、粒子更新与渲染
│   ├── fluid_sim.h
│   ├── accel_input.c    # 传感器自动识别与原始加速度采样
│   ├── accel_input.h
│   └── CMakeLists.txt
├── CMakeLists.txt       # 项目入口构建配置
├── partitions.csv       # 分区表
├── dependencies.lock    # 依赖锁定文件
├── CLAUDE.md            # 项目开发说明
├── README.md            # 默认中文文档
└── README_EN.md         # 英文文档
```

## 架构说明

### 数据流

```text
accel_input.c -> main.c -> watch_ui.c -> fluid_sim.c
```

### 输入处理流程

```text
原始加速度数据
-> 首帧偏置校准
-> 死区过滤
-> 倾角限幅
-> 输入曲线映射
-> 低通滤波
-> 驱动流体模拟
```

### 流体模拟实现

该项目的流体模块使用了简化 SPH（Smoothed Particle Hydrodynamics）方案，核心包括：

- 使用 **Poly6 kernel** 计算密度
- 使用 **Spiky gradient** 计算压力项
- 使用 **Viscous Laplacian** 计算粘性项
- 使用 **空间哈希网格** 优化邻域查找
- 使用 **XSPH 平滑** 提升粒子运动稳定性

主要参数集中定义在 `main/fluid_sim.c` 与 `main/main.c` 顶部，便于调参。

## 可调参数

### 流体输入参数（`main/main.c`）

```c
#define FLUID_INPUT_FILTER_ALPHA  0.80f
#define FLUID_INPUT_DEADZONE_G    0.03f
#define FLUID_INPUT_MAX_TILT_G    0.40f
#define FLUID_INPUT_CURVE_POWER   1.0f
```

### 流体物理参数（`main/fluid_sim.c`）

```c
#define FLUID_PARTICLE_COUNT      42
#define FLUID_STIFFNESS           2300.0f
#define FLUID_VISCOSITY           3.5f
#define FLUID_GRAVITY_SCALE       1800.0f
```

## 适用人群

- 想学习 **ESP-IDF + LVGL** 图形开发的开发者
- 想做 **传感器交互界面** 的嵌入式开发者
- 想参考 **轻量级流体效果** 实现方式的项目作者
- 使用 **ESP32-S3-EYE** 做 UI 展示或概念验证的爱好者

## 🤝 贡献指南

欢迎提交 Issue 或 Pull Request。

建议流程：

1. Fork 本仓库
2. 新建功能分支
3. 提交修改
4. 发起 Pull Request

## 📄 License

本项目采用 [MIT License](./LICENSE)。
