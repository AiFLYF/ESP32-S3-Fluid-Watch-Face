#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "watch_ui.h"
#include "accel_input.h"

static const char *TAG = "watch_face";

#define WIFI_SSID "431"
#define WIFI_PASS "88888888"
#define FLUID_UPDATE_PERIOD_MS 33
/* ── 输入参数 ──────────────────────────────────────────────── */
#define FLUID_INPUT_FILTER_ALPHA 0.80f    /* 高响应：几乎直通，仅做极轻平滑 */
#define FLUID_INPUT_DEADZONE_G   0.03f    /* 死区约1.7°，过滤微小抖动 */
#define FLUID_INPUT_MAX_TILT_G   0.40f    /* 满偏约23°，允许较大倾斜 */
#define FLUID_INPUT_CURVE_POWER  1.0f     /* 线性曲线：输入即所得，无延迟感 */
/* ─────────────────────────────────────────────────────────── */

static bool time_synced = false;
static bool wifi_connected = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized!");
    time_synced = true;
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init finished.");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    esp_sntp_init();
    
    setenv("TZ", "CST-8", 1);
    tzset();
}

static float clampf_local(const float value, const float min_value, const float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float apply_deadzone(const float value, const float deadzone)
{
    if (value > -deadzone && value < deadzone) {
        return 0.0f;
    }

    if (value > 0.0f) {
        return value - deadzone;
    }

    return value + deadzone;
}

static float apply_input_curve(const float value, const float max_val, const float power)
{
    if (fabsf(value) < 0.0001f) {
        return 0.0f;
    }
    const float normalized = fabsf(value) / max_val;
    const float curved = powf(normalized, power) * max_val;
    return (value > 0.0f) ? curved : -curved;
}

static void update_time_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (time_synced && wifi_connected) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        watch_ui_update_time(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        watch_ui_update_date(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, 
                             timeinfo.tm_mday, timeinfo.tm_wday);
    } else {
        static int demo_hour = 10;
        static int demo_min = 30;
        static int demo_sec = 0;
        
        demo_sec++;
        if (demo_sec >= 60) {
            demo_sec = 0;
            demo_min++;
            if (demo_min >= 60) {
                demo_min = 0;
                demo_hour++;
                if (demo_hour >= 24) {
                    demo_hour = 0;
                }
            }
        }
        
        watch_ui_update_time(demo_hour, demo_min, demo_sec);
        watch_ui_update_date(2026, 4, 8, 2);
    }
}

static void update_fluid_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    static float filtered_x  = 0.0f;
    static float filtered_y  = 0.0f;
    static float bias_x      = 0.0f;
    static float bias_y      = 0.0f;
    static bool  bias_ready  = false;
    accel_input_sample_t sample;

    if (!accel_input_poll(&sample) || !sample.valid) {
        return;
    }

    /*
     * 实测轴映射（SC7A20，板子侧立手持，X轴朝下约 -1g）：
     *   左右倾斜 → sample.y_g 变化（左倾正，右倾负）
     *   上下倾斜 → sample.x_g 变化（需减去 ~1g 基准）
     *
     * 首帧采集零点 bias，消除装配偏差。
     */
    if (!bias_ready) {
        bias_x    = sample.x_g;   /* ≈ 1g 基准 */
        bias_y    = sample.y_g;   /* ≈ 0  基准 */
        bias_ready = true;
        ESP_LOGI(TAG, "Tilt bias calibrated: x=%.3f y=%.3f", bias_x, bias_y);
    }

    float tilt_x = -(sample.y_g - bias_y);  /* 左右：左倾→负，液体向左流 */
    float tilt_y =  (sample.x_g - bias_x);  /* 上下：向上倾→液体向上流（取反修正） */

    /* 死区 */
    tilt_x = apply_deadzone(tilt_x, FLUID_INPUT_DEADZONE_G);
    tilt_y = apply_deadzone(tilt_y, FLUID_INPUT_DEADZONE_G);

    /* 限幅 */
    tilt_x = clampf_local(tilt_x, -FLUID_INPUT_MAX_TILT_G, FLUID_INPUT_MAX_TILT_G);
    tilt_y = clampf_local(tilt_y, -FLUID_INPUT_MAX_TILT_G, FLUID_INPUT_MAX_TILT_G);

    /* 输入曲线 */
    tilt_x = apply_input_curve(tilt_x, FLUID_INPUT_MAX_TILT_G, FLUID_INPUT_CURVE_POWER);
    tilt_y = apply_input_curve(tilt_y, FLUID_INPUT_MAX_TILT_G, FLUID_INPUT_CURVE_POWER);

    /* 低通滤波 */
    filtered_x += (tilt_x - filtered_x) * FLUID_INPUT_FILTER_ALPHA;
    filtered_y += (tilt_y - filtered_y) * FLUID_INPUT_FILTER_ALPHA;

    /* 小量归零，防止静止时液体持续微漂 */
    if (fabsf(filtered_x) < 0.01f) filtered_x = 0.0f;
    if (fabsf(filtered_y) < 0.01f) filtered_y = 0.0f;

    /* 诊断日志：每秒一次 */
    static uint32_t s_log_tick = 0;
    if (++s_log_tick >= 30) {
        s_log_tick = 0;
        ESP_LOGI(TAG,
                 "[FLUID] in=(%.3f,%.3f) raw=(%.3f,%.3f,%.3f) src=%s",
                 filtered_x, filtered_y,
                 sample.x_g, sample.y_g, sample.z_g,
                 sample.source_name ? sample.source_name : "?");
    }

    watch_ui_update_fluid(filtered_x, filtered_y, sample.source_name, FLUID_UPDATE_PERIOD_MS);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_display_start();
    
    ESP_LOGI(TAG, "Initializing watch face");
    
    bsp_display_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(NULL);
    watch_ui_init(scr);
    
    /* 隐藏 LVGL FPS/性能监视器 */
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_obj_t *sys_layer = lv_display_get_layer_sys(disp);
        uint32_t child_cnt = lv_obj_get_child_count(sys_layer);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_add_flag(lv_obj_get_child(sys_layer, i), LV_OBJ_FLAG_HIDDEN);
        }
    }
    bsp_display_unlock();
    
    bsp_display_backlight_on();

    ESP_ERROR_CHECK(accel_input_init());
    
    wifi_init_sta();
    initialize_sntp();
    
    lv_timer_create(update_time_timer_cb, 1000, NULL);
    lv_timer_create(update_fluid_timer_cb, FLUID_UPDATE_PERIOD_MS, NULL);
    
    ESP_LOGI(TAG, "Watch face started");
}
