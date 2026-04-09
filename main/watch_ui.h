#ifndef WATCH_UI_H
#define WATCH_UI_H

#include <stdint.h>
#include "lvgl.h"

void watch_ui_init(lv_obj_t *scr);
void watch_ui_update_time(int hour, int minute, int second);
void watch_ui_update_date(int year, int month, int day, int weekday);
void watch_ui_update_battery(int percent);
void watch_ui_update_fluid(float accel_x_g, float accel_y_g, const char *source_name, uint32_t dt_ms);

#endif
