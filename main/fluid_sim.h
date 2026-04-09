#ifndef FLUID_SIM_H
#define FLUID_SIM_H

#include <stdint.h>
#include "lvgl.h"

#define FLUID_SIM_CANVAS_SIZE 132

void fluid_sim_init(void);
void fluid_sim_step(float accel_x_g, float accel_y_g, uint32_t dt_ms);
void fluid_sim_render(lv_obj_t *canvas);

#endif
