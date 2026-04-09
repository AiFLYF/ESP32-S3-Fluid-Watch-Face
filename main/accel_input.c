#include "accel_input.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"

/* ── 可调参数 ──────────────────────────────────────────────── */
/* 注意：滤波和后处理已移至 main.c 统一处理，这里只输出原始数据 */
/* ─────────────────────────────────────────────────────────── */

#define ACCEL_I2C_TIMEOUT_MS  30
#define ACCEL_I2C_FREQ_HZ     400000
#define ACCEL_ALT_I2C_PORT    I2C_NUM_1

typedef enum {
    ACCEL_SOURCE_NONE = 0,
    ACCEL_SOURCE_DEMO,
    ACCEL_SOURCE_MPU6050,
    ACCEL_SOURCE_LIS3DH,   /* 也用于 SC7A20 */
    ACCEL_SOURCE_QMA7981,
    ACCEL_SOURCE_BUTTONS,
} accel_source_t;

typedef struct {
    uint8_t reg_base;
    uint8_t shift;
    float   lsb_per_g;
    bool    ready;
} qma_format_t;

static const char *TAG = "accel_input";

static i2c_master_bus_handle_t  s_i2c_bus      = NULL;
static bool                     s_i2c_bus_owned = false;
static const char              *s_i2c_bus_name  = "none";
static i2c_master_dev_handle_t  s_i2c_device   = NULL;
static accel_source_t           s_source        = ACCEL_SOURCE_NONE;
static const char              *s_source_name   = "Demo";
static button_handle_t          s_buttons[BSP_BUTTON_NUM] = {0};
static bool                     s_buttons_ready = false;

/* 原始数据状态（已移除滤波） */

static qma_format_t s_qma_format = {
    .reg_base  = 0x01,
    .shift     = 4,
    .lsb_per_g = 1024.0f,
    .ready     = false,
};

/* ── I2C 底层 ─────────────────────────────────────────────── */

static void accel_input_remove_device(void)
{
    if (s_i2c_device != NULL) {
        i2c_master_bus_rm_device(s_i2c_device);
        s_i2c_device = NULL;
    }
}

static void accel_input_release_owned_bus(void)
{
    accel_input_remove_device();
    if (s_i2c_bus_owned && s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
    }
    s_i2c_bus       = NULL;
    s_i2c_bus_owned = false;
    s_i2c_bus_name  = "none";
}

static esp_err_t accel_input_select_bsp_bus(void)
{
    accel_input_remove_device();
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) return ret;

    if (s_i2c_bus_owned && s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
    }
    s_i2c_bus       = bsp_i2c_get_handle();
    s_i2c_bus_owned = false;
    s_i2c_bus_name  = "BSP";
    return (s_i2c_bus != NULL) ? ESP_OK : ESP_FAIL;
}

static esp_err_t accel_input_add_device(const uint8_t address)
{
    if (s_i2c_bus == NULL) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = address,
        .scl_speed_hz    = ACCEL_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, &s_i2c_device);
}

static esp_err_t accel_input_read_reg(const uint8_t reg, uint8_t *data, const size_t len)
{
    return i2c_master_transmit_receive(s_i2c_device, &reg, 1, data, len, ACCEL_I2C_TIMEOUT_MS);
}

static esp_err_t accel_input_write_reg(const uint8_t reg, const uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(s_i2c_device, payload, 2, ACCEL_I2C_TIMEOUT_MS);
}

/* ── I2C 扫描（仅失败时用于诊断） ─────────────────────────── */

static void accel_input_log_i2c_scan(void)
{
    if (s_i2c_bus == NULL) return;
    ESP_LOGW(TAG, "Scanning %s I2C bus for unknown devices...", s_i2c_bus_name);

    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        if (i2c_master_probe(s_i2c_bus, addr, ACCEL_I2C_TIMEOUT_MS) != ESP_OK) continue;

        uint8_t r00 = 0, r0f = 0;
        const char *s00 = "n/a", *s0f = "n/a";
        accel_input_remove_device();
        if (accel_input_add_device(addr) == ESP_OK) {
            if (accel_input_read_reg(0x00, &r00, 1) == ESP_OK) s00 = "ok";
            if (accel_input_read_reg(0x0F, &r0f, 1) == ESP_OK) s0f = "ok";
        }
        ESP_LOGW(TAG, "  @0x%02X reg00(%s)=0x%02X reg0F(%s)=0x%02X",
                 addr, s00, r00, s0f, r0f);
    }
    accel_input_remove_device();
}

/* ── 通用数据输出（统一到屏幕坐标系）────────────────── */
/*
 * 屏幕坐标系定义（手持竖屏，屏幕朝向自己）：
 *   x_g : 屏幕右方为正（左右倾斜控制量）
 *   y_g : 屏幕下方为正（前后倾斜控制量，来自传感器 Z 轴）
 *   z_g : 屏幕法线朝外为正（来自传感器 Y 轴）
 *
 * SC7A20 / LIS3DH 装配方向：竖持时 raw_y ≈ -1g（重力轴），
 * 前后倾斜体现在 raw_z 上，故将 raw_z 映射为 y_g。
 * 如果方向不对只需在这里调整正负号，不影响上层逻辑。
 */
static void accel_apply_and_fill(float raw_x, float raw_y, float raw_z,
                                  accel_input_sample_t *sample)
{
    sample->x_g         =  raw_x;   /* 左右 */
    sample->y_g         =  raw_z;   /* 前后（来自 Z 轴） */
    sample->z_g         = -raw_y;   /* 法线（来自 Y 轴，取反使朝外为正） */
    sample->valid       = true;
    sample->source_name = s_source_name;
}

/* ── Demo / Buttons fallback ──────────────────────────────── */

static bool accel_input_fill_demo(accel_input_sample_t *sample)
{
    sample->x_g        = 0.0f;
    sample->y_g        = 0.0f;
    sample->z_g        = 1.0f;
    sample->valid      = true;
    sample->source_name = s_source_name;
    return true;
}

static bool accel_input_fill_buttons(accel_input_sample_t *sample)
{
    if (!s_buttons_ready) return accel_input_fill_demo(sample);

    const float x = (iot_button_get_key_level(s_buttons[BSP_BUTTON_4]) ? 0.35f : 0.0f) -
                    (iot_button_get_key_level(s_buttons[BSP_BUTTON_1]) ? 0.35f : 0.0f);
    const float y = (iot_button_get_key_level(s_buttons[BSP_BUTTON_3]) ? 0.35f : 0.0f) -
                    (iot_button_get_key_level(s_buttons[BSP_BUTTON_2]) ? 0.35f : 0.0f);

    sample->x_g        = x;
    sample->y_g        = y;
    sample->z_g        = 1.0f;
    sample->valid      = true;
    sample->source_name = s_source_name;
    return true;
}

static esp_err_t accel_input_init_buttons(void)
{
    esp_err_t ret = bsp_iot_button_create(s_buttons, NULL, BSP_BUTTON_NUM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to init buttons: %s", esp_err_to_name(ret));
        return ret;
    }
    s_buttons_ready = true;
    s_source        = ACCEL_SOURCE_BUTTONS;
    s_source_name   = "Buttons";
    ESP_LOGW(TAG, "No IMU found, using buttons as fallback");
    ESP_LOGI(TAG, "BTN1=left BTN2=up BTN3=down BTN4=right");
    return ESP_OK;
}

/* ── QMA7981 ──────────────────────────────────────────────── */

static bool accel_input_qma_block_is_zero(const uint8_t b[7])
{
    for (int i = 0; i < 7; ++i) if (b[i]) return false;
    return true;
}

static bool accel_input_qma_decode_block(const uint8_t raw[7], const uint8_t reg_base,
                                         const uint8_t shift, const float lsb,
                                         float *ox, float *oy, float *oz)
{
    const uint8_t off = (reg_base == 0x02) ? 1 : 0;
    const uint8_t *r  = &raw[off];
    *ox = (float)((int16_t)((((uint16_t)r[1]) << 8) | r[0]) >> shift) / lsb;
    *oy = (float)((int16_t)((((uint16_t)r[3]) << 8) | r[2]) >> shift) / lsb;
    *oz = (float)((int16_t)((((uint16_t)r[5]) << 8) | r[4]) >> shift) / lsb;
    return true;
}

static void accel_input_qma_select_format(const uint8_t raw[7])
{
    static const qma_format_t candidates[] = {
        {0x01, 2, 4096.0f, true}, {0x01, 4, 1024.0f, true}, {0x01, 6, 256.0f, true},
        {0x02, 2, 4096.0f, true}, {0x02, 4, 1024.0f, true}, {0x02, 6, 256.0f, true},
    };
    float best_score = 1e9f;
    qma_format_t best = s_qma_format;

    for (size_t i = 0; i < 6; ++i) {
        float x, y, z;
        accel_input_qma_decode_block(raw, candidates[i].reg_base,
                                     candidates[i].shift, candidates[i].lsb_per_g,
                                     &x, &y, &z);
        float mag   = sqrtf(x*x + y*y + z*z);
        float score = fabsf(mag - 1.0f);
        if (mag < 0.20f || mag > 3.0f) score += 3.0f;
        if (candidates[i].reg_base == 0x01) score -= 0.02f;
        if (score < best_score) { best_score = score; best = candidates[i]; }
    }
    s_qma_format       = best;
    s_qma_format.ready = true;
}

static bool accel_input_try_probe_qma7981(const uint8_t address)
{
    uint8_t r00 = 0, r0f = 0;
    accel_input_remove_device();
    if (accel_input_add_device(address) != ESP_OK) return false;
    if (accel_input_read_reg(0x00, &r00, 1) != ESP_OK ||
        accel_input_read_reg(0x0F, &r0f, 1) != ESP_OK ||
        (r00 != 0xE7 && r0f != 0x11)) {
        accel_input_remove_device();
        return false;
    }

    /* 如果 reg0F=0x11 但 reg00≠0xE7，可能是 SC7A20，由后续探测处理 */
    if (r00 != 0xE7) {
        accel_input_remove_device();
        return false;
    }

    ESP_LOGI(TAG, "QMA7981 found @0x%02X, initializing...", address);
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x10, 0x05)); /* ODR 100Hz */
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x11, 0x01)); /* normal */
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t blk[7] = {0};
    if (accel_input_read_reg(0x01, blk, 7) != ESP_OK || accel_input_qma_block_is_zero(blk)) {
        ESP_LOGW(TAG, "QMA7981 @0x%02X data zero after init, skip", address);
        accel_input_remove_device();
        return false;
    }
    s_qma_format.ready = false;
    accel_input_qma_select_format(blk);
    s_source      = ACCEL_SOURCE_QMA7981;
    s_source_name = "QMA7981";
    ESP_LOGI(TAG, "Detected: %s @0x%02X on %s (shift=%u scale=%.0f)",
             s_source_name, address, s_i2c_bus_name,
             s_qma_format.shift, s_qma_format.lsb_per_g);
    return true;
}

/* ── MPU6050 ──────────────────────────────────────────────── */

static bool accel_input_try_probe_mpu6050(const uint8_t address)
{
    uint8_t who = 0;
    accel_input_remove_device();
    if (accel_input_add_device(address) != ESP_OK) return false;
    if (accel_input_read_reg(0x75, &who, 1) != ESP_OK ||
        (who != 0x68 && who != 0x70 && who != 0x71)) {
        accel_input_remove_device();
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x6B, 0x00)); /* wake */
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x1A, 0x03)); /* DLPF 44Hz */
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x1B, 0x00)); /* gyro ±250 */
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x1C, 0x00)); /* accel ±2g */
    s_source      = ACCEL_SOURCE_MPU6050;
    s_source_name = "MPU6050";
    ESP_LOGI(TAG, "Detected: %s @0x%02X on %s", s_source_name, address, s_i2c_bus_name);
    return true;
}

/* ── LIS3DH / SC7A20 ─────────────────────────────────────── */

static bool accel_input_try_probe_lis3dh_family(const uint8_t address)
{
    uint8_t who = 0;
    accel_input_remove_device();
    if (accel_input_add_device(address) != ESP_OK) return false;
    if (accel_input_read_reg(0x0F, &who, 1) != ESP_OK) {
        accel_input_remove_device();
        return false;
    }
    /* 0x33=LIS3DH  0x11=SC7A20 */
    if (who != 0x33 && who != 0x11) {
        accel_input_remove_device();
        return false;
    }

    /*
     * CTRL_REG1 0x20 = 0x77 → ODR 400Hz，所有轴使能（正常功耗模式）
     * CTRL_REG4 0x23 = 0x88 → BDU=1, HR=1（高精度），±2g
     */
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x20, 0x77));
    ESP_ERROR_CHECK_WITHOUT_ABORT(accel_input_write_reg(0x23, 0x88));
    vTaskDelay(pdMS_TO_TICKS(10));

    s_source      = ACCEL_SOURCE_LIS3DH;
    s_source_name = (who == 0x11) ? "SC7A20" : "LIS3DH";
    ESP_LOGI(TAG, "Detected: %s (WHO=0x%02X) @0x%02X on %s",
             s_source_name, who, address, s_i2c_bus_name);
    return true;
}

/* ── 探测入口 ─────────────────────────────────────────────── */

static bool accel_input_try_supported_sensors_on_current_bus(void)
{
    static const uint8_t qma_addrs[] = {0x18, 0x19, 0x12, 0x13, 0x14};
    for (size_t i = 0; i < sizeof(qma_addrs); ++i) {
        if (accel_input_try_probe_qma7981(qma_addrs[i])) return true;
    }
    return accel_input_try_probe_mpu6050(0x68)          ||
           accel_input_try_probe_mpu6050(0x69)          ||
           accel_input_try_probe_lis3dh_family(0x18)    ||
           accel_input_try_probe_lis3dh_family(0x19);
}

esp_err_t accel_input_init(void)
{
    esp_err_t ret = accel_input_select_bsp_bus();
    if (ret == ESP_OK && accel_input_try_supported_sensors_on_current_bus()) {
        return ESP_OK;
    }
    if (ret == ESP_OK) accel_input_log_i2c_scan();
    accel_input_release_owned_bus();
    return accel_input_init_buttons();
}

/* ── poll（对外接口）─────────────────────────────────────── */

bool accel_input_poll(accel_input_sample_t *sample)
{
    if (sample == NULL) return false;
    memset(sample, 0, sizeof(*sample));

    if (s_source == ACCEL_SOURCE_MPU6050) {
        uint8_t raw[6] = {0};
        if (accel_input_read_reg(0x3B, raw, 6) == ESP_OK) {
            const float rx = (float)(int16_t)((raw[0] << 8) | raw[1]) / 16384.0f;
            const float ry = (float)(int16_t)((raw[2] << 8) | raw[3]) / 16384.0f;
            const float rz = (float)(int16_t)((raw[4] << 8) | raw[5]) / 16384.0f;
            accel_apply_and_fill(rx, ry, rz, sample);
            return true;
        }

    } else if (s_source == ACCEL_SOURCE_LIS3DH) {
        uint8_t raw[6] = {0};
        /* 0x28|0x80 : OUT_X_L，auto-increment */
        if (accel_input_read_reg(0x28 | 0x80, raw, 6) == ESP_OK) {
            /* 高精度模式：16bit 左对齐，右移4位得12bit有效值，±2g→1mg/digit */
            const float rx = (float)((int16_t)(((uint16_t)raw[1] << 8) | raw[0]) >> 4) / 1024.0f;
            const float ry = (float)((int16_t)(((uint16_t)raw[3] << 8) | raw[2]) >> 4) / 1024.0f;
            const float rz = (float)((int16_t)(((uint16_t)raw[5] << 8) | raw[4]) >> 4) / 1024.0f;
            accel_apply_and_fill(rx, ry, rz, sample);
            return true;
        }

    } else if (s_source == ACCEL_SOURCE_QMA7981) {
        uint8_t blk[7] = {0};
        if (accel_input_read_reg(0x01, blk, 7) == ESP_OK &&
            !accel_input_qma_block_is_zero(blk)) {
            if (!s_qma_format.ready) accel_input_qma_select_format(blk);
            float rx, ry, rz;
            if (accel_input_qma_decode_block(blk, s_qma_format.reg_base,
                                              s_qma_format.shift, s_qma_format.lsb_per_g,
                                              &rx, &ry, &rz)) {
                accel_apply_and_fill(rx, ry, rz, sample);
                return true;
            }
        }

    } else if (s_source == ACCEL_SOURCE_BUTTONS) {
        return accel_input_fill_buttons(sample);
    }

    return accel_input_fill_demo(sample);
}