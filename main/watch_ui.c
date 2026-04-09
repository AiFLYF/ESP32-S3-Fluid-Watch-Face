#include <stdio.h>
#include <math.h>
#include "lvgl.h"
#include "fluid_sim.h"

#ifndef PI
#define PI (3.14159f)
#endif

#define WATCH_OPA(percent) ((lv_opa_t)(((percent) * 255 + 50) / 100))

#define WATCH_CENTER_X 120
#define WATCH_CENTER_Y 120
#define WATCH_RADIUS 110
#define WATCH_INFO_X_OFFSET 0
#define WATCH_DATE_Y_OFFSET (-39)
#define WATCH_TIME_Y_OFFSET 60
#define WATCH_FLUID_Y_OFFSET (-1)

static lv_obj_t *hour_label;
static lv_obj_t *minute_label;
static lv_obj_t *second_label;
static lv_obj_t *date_label;
static lv_obj_t *weekday_label;
static lv_obj_t *battery_arc;   /* unused, kept for ABI */
static lv_obj_t *battery_label; /* unused, kept for ABI */
static lv_obj_t *hour_hand;
static lv_obj_t *minute_hand;
static lv_obj_t *second_hand;
static lv_obj_t *s_clock_bg;
static lv_obj_t *fluid_canvas;

LV_DRAW_BUF_DEFINE_STATIC(s_fluid_draw_buf, FLUID_SIM_CANVAS_SIZE, FLUID_SIM_CANVAS_SIZE, LV_COLOR_FORMAT_ARGB8888);

static const char *weekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

static void style_info_panel(lv_obj_t *panel,
                             lv_color_t bg_color,
                             lv_opa_t bg_opa,
                             lv_color_t border_color,
                             lv_opa_t border_opa,
                             lv_coord_t radius)
{
    lv_obj_set_style_bg_color(panel, bg_color, 0);
    lv_obj_set_style_bg_opa(panel, bg_opa, 0);
    lv_obj_set_style_border_color(panel, border_color, 0);
    lv_obj_set_style_border_opa(panel, border_opa, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_pad_left(panel, 8, 0);
    lv_obj_set_style_pad_right(panel, 8, 0);
    lv_obj_set_style_pad_top(panel, 3, 0);
    lv_obj_set_style_pad_bottom(panel, 3, 0);
    lv_obj_set_style_shadow_width(panel, 14, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_make(18, 72, 140), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_20, 0);
    lv_obj_set_style_shadow_ofs_x(panel, 0, 0);
    lv_obj_set_style_shadow_ofs_y(panel, 0, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_ring(lv_obj_t *parent,
                        lv_coord_t size,
                        lv_color_t border_color,
                        lv_opa_t border_opa,
                        lv_coord_t border_width,
                        lv_coord_t y_ofs)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, size, size);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, border_color, 0);
    lv_obj_set_style_border_opa(ring, border_opa, 0);
    lv_obj_set_style_border_width(ring, border_width, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_set_scrollbar_mode(ring, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, y_ofs);
}

static void create_line_segment(lv_obj_t *parent, int x1, int y1, int x2, int y2, lv_color_t color, int width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_point_precise_t *pts = (lv_point_precise_t *)lv_malloc(2 * sizeof(lv_point_precise_t));
    if (pts == NULL) {
        return;
    }

    pts[0].x = x1;
    pts[0].y = y1;
    pts[1].x = x2;
    pts[1].y = y2;
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
}

static void create_clock_face(lv_obj_t *scr)
{
    s_clock_bg = lv_obj_create(scr);
    lv_obj_t *clock_bg = s_clock_bg;
    lv_obj_set_size(clock_bg, 240, 240);
    lv_obj_set_pos(clock_bg, 0, 0);
    lv_obj_set_style_bg_color(clock_bg, lv_color_make(2, 6, 18), 0);
    lv_obj_set_style_bg_opa(clock_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clock_bg, lv_color_make(68, 112, 168), 0);
    lv_obj_set_style_border_opa(clock_bg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(clock_bg, 2, 0);
    lv_obj_set_style_radius(clock_bg, 120, 0);
    lv_obj_set_style_pad_all(clock_bg, 0, 0);
    lv_obj_set_style_shadow_width(clock_bg, 22, 0);
    lv_obj_set_style_shadow_color(clock_bg, lv_color_make(0, 72, 150), 0);
    lv_obj_set_style_shadow_opa(clock_bg, WATCH_OPA(25), 0);
    lv_obj_set_style_shadow_ofs_x(clock_bg, 0, 0);
    lv_obj_set_style_shadow_ofs_y(clock_bg, 0, 0);
    lv_obj_set_scrollbar_mode(clock_bg, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(clock_bg, LV_OBJ_FLAG_SCROLLABLE);

    create_ring(clock_bg, 224, lv_color_make(34, 64, 108), LV_OPA_70, 1, 0);

    for (int i = 0; i < 60; i++) {
        const float angle = i * 6.0f * PI / 180.0f;
        const bool is_hour_tick = (i % 5 == 0);
        const bool is_cardinal_tick = (i % 15 == 0);
        const int inner_r = is_cardinal_tick ? 91 : (is_hour_tick ? 95 : 103);
        const int outer_r = is_cardinal_tick ? WATCH_RADIUS : (is_hour_tick ? 109 : 112);
        const int width = is_cardinal_tick ? 5 : (is_hour_tick ? 3 : 1);
        const lv_color_t color = is_cardinal_tick ? lv_color_make(176, 216, 255) :
                                                  (is_hour_tick ? lv_color_make(130, 176, 232) : lv_color_make(66, 84, 110));

        create_line_segment(clock_bg,
                            WATCH_CENTER_X + (int)(inner_r * sinf(angle)),
                            WATCH_CENTER_Y - (int)(inner_r * cosf(angle)),
                            WATCH_CENTER_X + (int)(outer_r * sinf(angle)),
                            WATCH_CENTER_Y - (int)(outer_r * cosf(angle)),
                            color, width);
    }

    for (int i = 0; i < 12; i++) {
        const float angle = i * 30.0f * PI / 180.0f;
        const int x = WATCH_CENTER_X + (int)(82 * sinf(angle)) - 11;
        const int y = WATCH_CENTER_Y - (int)(82 * cosf(angle)) - 9;
        const bool is_cardinal_number = (i % 3 == 0);

        lv_obj_t *num_label = lv_label_create(clock_bg);
        char num_str[4];
        snprintf(num_str, sizeof(num_str), "%d", i == 0 ? 12 : i);
        lv_label_set_text(num_label, num_str);
        lv_obj_set_size(num_label, 22, 18);
        lv_obj_set_style_bg_opa(num_label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(num_label, 0, 0);
        lv_obj_set_style_pad_all(num_label, 0, 0);
        lv_obj_set_style_text_align(num_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(num_label, is_cardinal_number ? lv_color_make(246, 248, 255) : lv_color_make(228, 236, 250), 0);
        lv_obj_set_style_text_opa(num_label, is_cardinal_number ? LV_OPA_COVER : WATCH_OPA(88), 0);
        lv_obj_set_style_text_font(num_label, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(num_label, x, y);
    }

    lv_obj_t *inner_glow = lv_obj_create(clock_bg);
    lv_obj_set_size(inner_glow, 154, 154);
    lv_obj_set_style_radius(inner_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner_glow, lv_color_make(8, 16, 34), 0);
    lv_obj_set_style_bg_opa(inner_glow, WATCH_OPA(18), 0);
    lv_obj_set_style_border_width(inner_glow, 0, 0);
    lv_obj_set_style_shadow_width(inner_glow, 18, 0);
    lv_obj_set_style_shadow_color(inner_glow, lv_color_make(10, 90, 180), 0);
    lv_obj_set_style_shadow_opa(inner_glow, WATCH_OPA(12), 0);
    lv_obj_set_style_pad_all(inner_glow, 0, 0);
    lv_obj_set_scrollbar_mode(inner_glow, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(inner_glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(inner_glow, LV_ALIGN_CENTER, 0, WATCH_FLUID_Y_OFFSET);

    create_ring(clock_bg, 148, lv_color_make(54, 112, 178), WATCH_OPA(48), 2, WATCH_FLUID_Y_OFFSET);
    create_ring(clock_bg, 138, lv_color_make(128, 182, 240), WATCH_OPA(18), 1, WATCH_FLUID_Y_OFFSET);
}

static void create_fluid_layer(void)
{
    LV_DRAW_BUF_INIT_STATIC(s_fluid_draw_buf);

    fluid_canvas = lv_canvas_create(s_clock_bg);
    lv_obj_set_size(fluid_canvas, FLUID_SIM_CANVAS_SIZE, FLUID_SIM_CANVAS_SIZE);
    lv_obj_align(fluid_canvas, LV_ALIGN_CENTER, 0, WATCH_FLUID_Y_OFFSET);
    lv_obj_set_style_bg_opa(fluid_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fluid_canvas, 0, 0);
    lv_obj_set_style_pad_all(fluid_canvas, 0, 0);
    lv_obj_clear_flag(fluid_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_draw_buf(fluid_canvas, &s_fluid_draw_buf);

    fluid_sim_init();
    fluid_sim_render(fluid_canvas);
}

static void create_clock_hands(lv_obj_t *scr)
{
    (void)scr;
    hour_hand = lv_line_create(s_clock_bg);
    minute_hand = lv_line_create(s_clock_bg);
    second_hand = lv_line_create(s_clock_bg);

    lv_obj_set_style_line_width(hour_hand, 6, 0);
    lv_obj_set_style_line_color(hour_hand, lv_color_make(255, 220, 152), 0);
    lv_obj_set_style_line_rounded(hour_hand, true, 0);

    lv_obj_set_style_line_width(minute_hand, 4, 0);
    lv_obj_set_style_line_color(minute_hand, lv_color_make(232, 244, 255), 0);
    lv_obj_set_style_line_rounded(minute_hand, true, 0);

    lv_obj_set_style_line_width(second_hand, 2, 0);
    lv_obj_set_style_line_color(second_hand, lv_color_make(255, 128, 138), 0);
    lv_obj_set_style_line_rounded(second_hand, true, 0);

    lv_obj_t *center_glow = lv_obj_create(s_clock_bg);
    lv_obj_set_size(center_glow, 32, 32);
    lv_obj_set_style_radius(center_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_glow, lv_color_make(28, 84, 150), 0);
    lv_obj_set_style_bg_opa(center_glow, WATCH_OPA(18), 0);
    lv_obj_set_style_border_width(center_glow, 0, 0);
    lv_obj_set_style_shadow_width(center_glow, 18, 0);
    lv_obj_set_style_shadow_color(center_glow, lv_color_make(24, 96, 180), 0);
    lv_obj_set_style_shadow_opa(center_glow, WATCH_OPA(18), 0);
    lv_obj_set_style_pad_all(center_glow, 0, 0);
    lv_obj_set_scrollbar_mode(center_glow, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(center_glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(center_glow, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *center_ring = lv_obj_create(s_clock_bg);
    lv_obj_set_size(center_ring, 20, 20);
    lv_obj_set_style_radius(center_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_ring, lv_color_make(8, 14, 26), 0);
    lv_obj_set_style_bg_opa(center_ring, LV_OPA_90, 0);
    lv_obj_set_style_border_color(center_ring, lv_color_make(150, 178, 220), 0);
    lv_obj_set_style_border_opa(center_ring, LV_OPA_80, 0);
    lv_obj_set_style_border_width(center_ring, 2, 0);
    lv_obj_set_style_shadow_width(center_ring, 10, 0);
    lv_obj_set_style_shadow_color(center_ring, lv_color_make(22, 90, 168), 0);
    lv_obj_set_style_shadow_opa(center_ring, WATCH_OPA(18), 0);
    lv_obj_set_style_pad_all(center_ring, 0, 0);
    lv_obj_set_scrollbar_mode(center_ring, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(center_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(center_ring, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *center_dot = lv_obj_create(center_ring);
    lv_obj_set_size(center_dot, 8, 8);
    lv_obj_set_style_radius(center_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_dot, lv_color_make(240, 246, 255), 0);
    lv_obj_set_style_bg_opa(center_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(center_dot, 0, 0);
    lv_obj_set_style_pad_all(center_dot, 0, 0);
    lv_obj_center(center_dot);
}

static void create_digital_time(lv_obj_t *scr)
{
    (void)scr;

    lv_obj_t *time_container = lv_obj_create(s_clock_bg);
    lv_obj_set_size(time_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(time_container, LV_ALIGN_CENTER, WATCH_INFO_X_OFFSET, WATCH_TIME_Y_OFFSET);
    lv_obj_set_layout(time_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(time_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    style_info_panel(time_container,
                     lv_color_make(7, 18, 36),
                     WATCH_OPA(45),
                     lv_color_make(90, 154, 224),
                     WATCH_OPA(35),
                     14);
    lv_obj_set_style_pad_left(time_container, 9, 0);
    lv_obj_set_style_pad_right(time_container, 9, 0);
    lv_obj_set_style_pad_top(time_container, 2, 0);
    lv_obj_set_style_pad_bottom(time_container, 2, 0);
    lv_obj_set_style_pad_column(time_container, 1, 0);

    hour_label = lv_label_create(time_container);
    lv_label_set_text(hour_label, "00");
    lv_obj_set_style_text_color(hour_label, lv_color_make(76, 238, 214), 0);
    lv_obj_set_style_text_font(hour_label, &lv_font_montserrat_14, 0);

    lv_obj_t *colon1 = lv_label_create(time_container);
    lv_label_set_text(colon1, ":");
    lv_obj_set_style_text_color(colon1, lv_color_make(230, 240, 255), 0);
    lv_obj_set_style_text_opa(colon1, WATCH_OPA(92), 0);
    lv_obj_set_style_text_font(colon1, &lv_font_montserrat_14, 0);

    minute_label = lv_label_create(time_container);
    lv_label_set_text(minute_label, "00");
    lv_obj_set_style_text_color(minute_label, lv_color_make(92, 232, 226), 0);
    lv_obj_set_style_text_font(minute_label, &lv_font_montserrat_14, 0);

    lv_obj_t *colon2 = lv_label_create(time_container);
    lv_label_set_text(colon2, ":");
    lv_obj_set_style_text_color(colon2, lv_color_make(132, 148, 176), 0);
    lv_obj_set_style_text_opa(colon2, LV_OPA_80, 0);
    lv_obj_set_style_text_font(colon2, &lv_font_montserrat_14, 0);

    second_label = lv_label_create(time_container);
    lv_label_set_text(second_label, "00");
    lv_obj_set_style_text_color(second_label, lv_color_make(160, 176, 196), 0);
    lv_obj_set_style_text_opa(second_label, WATCH_OPA(76), 0);
    lv_obj_set_style_text_font(second_label, &lv_font_montserrat_14, 0);
}

static void create_date_display(lv_obj_t *scr)
{
    (void)scr;

    lv_obj_t *date_container = lv_obj_create(s_clock_bg);
    lv_obj_set_size(date_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(date_container, LV_ALIGN_CENTER, WATCH_INFO_X_OFFSET, WATCH_DATE_Y_OFFSET);
    lv_obj_set_layout(date_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(date_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(date_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    style_info_panel(date_container,
                     lv_color_make(8, 18, 34),
                     WATCH_OPA(34),
                     lv_color_make(82, 138, 206),
                     WATCH_OPA(28),
                     16);
    lv_obj_set_style_pad_left(date_container, 10, 0);
    lv_obj_set_style_pad_right(date_container, 10, 0);
    lv_obj_set_style_pad_top(date_container, 4, 0);
    lv_obj_set_style_pad_bottom(date_container, 4, 0);
    lv_obj_set_style_pad_row(date_container, 0, 0);

    weekday_label = lv_label_create(date_container);
    lv_label_set_text(weekday_label, "TUE");
    lv_obj_set_style_text_color(weekday_label, lv_color_make(98, 208, 255), 0);
    lv_obj_set_style_text_opa(weekday_label, WATCH_OPA(92), 0);
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_14, 0);

    date_label = lv_label_create(date_container);
    lv_label_set_text(date_label, "2026-04-08");
    lv_obj_set_style_text_color(date_label, lv_color_make(176, 186, 202), 0);
    lv_obj_set_style_text_opa(date_label, WATCH_OPA(78), 0);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
}

static void create_battery_indicator(lv_obj_t *scr)
{
    (void)scr;
    battery_arc   = NULL;
    battery_label = NULL;
}

static lv_point_precise_t hour_pts[2];
static lv_point_precise_t minute_pts[2];
static lv_point_precise_t second_pts[2];

static void update_hand_position(lv_obj_t *hand, lv_point_precise_t *pts, float angle_deg, int length)
{
    float angle_rad = angle_deg * PI / 180.0f;
    
    pts[0].x = WATCH_CENTER_X;
    pts[0].y = WATCH_CENTER_Y;
    pts[1].x = WATCH_CENTER_X + (int)(length * sinf(angle_rad));
    pts[1].y = WATCH_CENTER_Y - (int)(length * cosf(angle_rad));
    
    lv_line_set_points(hand, pts, 2);
}

void watch_ui_init(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    
    create_clock_face(scr);
    create_fluid_layer();
    create_clock_hands(scr);
    create_digital_time(scr);
    create_date_display(scr);
    create_battery_indicator(scr);
}

void watch_ui_update_time(int hour, int minute, int second)
{
    static char buf[8];
    
    snprintf(buf, sizeof(buf), "%02d", hour);
    lv_label_set_text(hour_label, buf);
    
    snprintf(buf, sizeof(buf), "%02d", minute);
    lv_label_set_text(minute_label, buf);
    
    snprintf(buf, sizeof(buf), "%02d", second);
    lv_label_set_text(second_label, buf);
    
    float hour_angle = (hour % 12) * 30.0f + minute * 0.5f;
    float minute_angle = minute * 6.0f + second * 0.1f;
    float second_angle = second * 6.0f;
    
    update_hand_position(hour_hand, hour_pts, hour_angle, 45);
    update_hand_position(minute_hand, minute_pts, minute_angle, 65);
    update_hand_position(second_hand, second_pts, second_angle, 75);
}

void watch_ui_update_date(int year, int month, int day, int weekday)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    lv_label_set_text(date_label, buf);
    
    if (weekday >= 0 && weekday < 7) {
        lv_label_set_text(weekday_label, weekdays[weekday]);
    }
}

void watch_ui_update_battery(int percent)
{
    (void)percent;
}

void watch_ui_update_fluid(float accel_x_g, float accel_y_g, const char *source_name, uint32_t dt_ms)
{
    (void)source_name;

    if (fluid_canvas == NULL) {
        return;
    }

    fluid_sim_step(accel_x_g, accel_y_g, dt_ms);
    fluid_sim_render(fluid_canvas);
}
