#ifndef ACCEL_INPUT_H
#define ACCEL_INPUT_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float x_g;
    float y_g;
    float z_g;
    bool valid;
    const char *source_name;
} accel_input_sample_t;

esp_err_t accel_input_init(void);
bool accel_input_poll(accel_input_sample_t *sample);

#endif
