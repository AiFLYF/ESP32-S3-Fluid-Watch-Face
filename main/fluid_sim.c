#include "fluid_sim.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define FLUID_PARTICLE_COUNT 42
#define FLUID_PARTICLE_DRAW_RADIUS 4
#define FLUID_PARTICLE_RADIUS 4.0f
#define FLUID_SUPPORT_RADIUS 16.0f
#define FLUID_SUPPORT_RADIUS_SQ (FLUID_SUPPORT_RADIUS * FLUID_SUPPORT_RADIUS)
#define FLUID_PARTICLE_MASS 650.0f
#define FLUID_REST_DENSITY 7.4f
#define FLUID_STIFFNESS 2300.0f
#define FLUID_PRESSURE_CLAMP 9000.0f
#define FLUID_VISCOSITY 3.5f       /* 低粘度：快速响应倾斜，减少内部拖拽感 */
#define FLUID_XSPH_C 0.025f        /* 速度平滑：降低以减少速度修正带来的延迟 */
#define FLUID_GRAVITY_SCALE 1800.0f /* 重力放大：0.3g → 540 px/s²，跟手响应 */

#define FLUID_CONTAINER_RADIUS 58.0f
#define FLUID_WALL_BOUNCE 0.10f    /* 弹性适中 */
#define FLUID_WALL_DAMPING 0.55f   /* 碰壁后快速减速，防止反复弹跳 */
#define FLUID_VELOCITY_DAMPING 0.985f /* 适度衰减：静止时不会无限漂移 */
#define FLUID_MAX_ACCEL_G 0.50f    /* 配合 main.c 的0.40g上限 */
#define FLUID_MAX_SPEED 160.0f     /* 提高速度上限：允许快速流动 */
#define FLUID_MAX_FORCE_ACCEL 2000.0f /* 放宽力上限(原1500) */
#define FLUID_COLLISION_DISTANCE (FLUID_PARTICLE_RADIUS * 2.15f)
#define FLUID_COLLISION_DISTANCE_SQ (FLUID_COLLISION_DISTANCE * FLUID_COLLISION_DISTANCE)
#define FLUID_POSITION_SOLVER_ITERS 2
#define FLUID_CELL_SIZE ((int)FLUID_SUPPORT_RADIUS)
#define FLUID_GRID_W ((FLUID_SIM_CANVAS_SIZE + FLUID_CELL_SIZE - 1) / FLUID_CELL_SIZE)
#define FLUID_GRID_H ((FLUID_SIM_CANVAS_SIZE + FLUID_CELL_SIZE - 1) / FLUID_CELL_SIZE)
#define FLUID_GRID_CELLS (FLUID_GRID_W * FLUID_GRID_H)


typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    float density;
    float pressure;
    float ax;
    float ay;
    float xsph_x;
    float xsph_y;
    int16_t next;
} fluid_particle_t;

static fluid_particle_t s_particles[FLUID_PARTICLE_COUNT];
static int16_t s_cell_head[FLUID_GRID_CELLS];
static bool s_initialized = false;
static const char *TAG_FLUID = "fluid_sim";

static float fluid_clampf(const float value, const float min_value, const float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float fluid_poly6_kernel(const float r_sq)
{
    if (r_sq >= FLUID_SUPPORT_RADIUS_SQ) {
        return 0.0f;
    }

    const float diff = FLUID_SUPPORT_RADIUS_SQ - r_sq;
    const float coeff = 4.0f / (M_PI * powf(FLUID_SUPPORT_RADIUS, 8.0f));
    return coeff * diff * diff * diff;
}

static float fluid_spiky_grad_kernel(const float r)
{
    if (r <= 0.0f || r >= FLUID_SUPPORT_RADIUS) {
        return 0.0f;
    }

    const float diff = FLUID_SUPPORT_RADIUS - r;
    const float coeff = 30.0f / (M_PI * powf(FLUID_SUPPORT_RADIUS, 5.0f));
    return coeff * diff * diff;
}

static float fluid_visc_laplacian_kernel(const float r)
{
    if (r >= FLUID_SUPPORT_RADIUS) {
        return 0.0f;
    }

    const float coeff = 20.0f / (3.0f * M_PI * powf(FLUID_SUPPORT_RADIUS, 5.0f));
    return coeff * (FLUID_SUPPORT_RADIUS - r);
}

static int fluid_cell_index(const int cell_x, const int cell_y)
{
    return cell_y * FLUID_GRID_W + cell_x;
}

static void fluid_build_grid(void)
{
    for (int i = 0; i < FLUID_GRID_CELLS; ++i) {
        s_cell_head[i] = -1;
    }

    for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
        const int cell_x = (int)fluid_clampf(floorf(s_particles[i].x / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_W - 1));
        const int cell_y = (int)fluid_clampf(floorf(s_particles[i].y / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_H - 1));
        const int cell_index = fluid_cell_index(cell_x, cell_y);

        s_particles[i].next = s_cell_head[cell_index];
        s_cell_head[cell_index] = (int16_t)i;
    }
}

static void fluid_apply_boundary(fluid_particle_t *particle)
{
    const float center = FLUID_SIM_CANVAS_SIZE * 0.5f;
    const float limit = FLUID_CONTAINER_RADIUS - FLUID_PARTICLE_DRAW_RADIUS - 1.0f;
    float dx = particle->x - center;
    float dy = particle->y - center;
    const float dist_sq = dx * dx + dy * dy;

    if (dist_sq <= limit * limit) {
        return;
    }

    float dist = sqrtf(dist_sq);
    if (dist < 0.001f) {
        dist = 0.001f;
        dx = limit;
        dy = 0.0f;
    }

    const float nx = dx / dist;
    const float ny = dy / dist;
    particle->x = center + nx * limit;
    particle->y = center + ny * limit;

    const float vn = particle->vx * nx + particle->vy * ny;
    if (vn > 0.0f) {
        particle->vx -= (1.0f + FLUID_WALL_BOUNCE) * vn * nx;
        particle->vy -= (1.0f + FLUID_WALL_BOUNCE) * vn * ny;
    }

    particle->vx *= FLUID_WALL_DAMPING;
    particle->vy *= FLUID_WALL_DAMPING;
}

static void fluid_limit_velocity(fluid_particle_t *particle)
{
    const float speed_sq = particle->vx * particle->vx + particle->vy * particle->vy;
    if (speed_sq <= FLUID_MAX_SPEED * FLUID_MAX_SPEED) {
        return;
    }

    const float scale = FLUID_MAX_SPEED / sqrtf(speed_sq);
    particle->vx *= scale;
    particle->vy *= scale;
}

static void fluid_relax_particles(void)
{
    for (int iter = 0; iter < FLUID_POSITION_SOLVER_ITERS; ++iter) {
        fluid_build_grid();

        for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
            fluid_particle_t *pi = &s_particles[i];
            const int base_cell_x = (int)fluid_clampf(floorf(pi->x / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_W - 1));
            const int base_cell_y = (int)fluid_clampf(floorf(pi->y / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_H - 1));

            for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                const int cell_y = base_cell_y + offset_y;
                if (cell_y < 0 || cell_y >= FLUID_GRID_H) {
                    continue;
                }

                for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                    const int cell_x = base_cell_x + offset_x;
                    if (cell_x < 0 || cell_x >= FLUID_GRID_W) {
                        continue;
                    }

                    for (int16_t j = s_cell_head[fluid_cell_index(cell_x, cell_y)]; j >= 0; j = s_particles[j].next) {
                        if (j <= i) {
                            continue;
                        }

                        fluid_particle_t *pj = &s_particles[j];
                        const float dx = pj->x - pi->x;
                        const float dy = pj->y - pi->y;
                        const float r_sq = dx * dx + dy * dy;
                        if (r_sq >= FLUID_COLLISION_DISTANCE_SQ) {
                            continue;
                        }

                        float dist = sqrtf(r_sq);
                        float nx;
                        float ny;
                        if (dist < 0.0001f) {
                            const float angle = ((float)(i * 17 + j * 13) / (float)FLUID_PARTICLE_COUNT) * 2.0f * M_PI;
                            nx = cosf(angle);
                            ny = sinf(angle);
                            dist = 0.0f;
                        } else {
                            nx = dx / dist;
                            ny = dy / dist;
                        }

                        const float overlap = FLUID_COLLISION_DISTANCE - dist;
                        const float correction = overlap * 0.425f;
                        pi->x -= nx * correction;
                        pi->y -= ny * correction;
                        pj->x += nx * correction;
                        pj->y += ny * correction;

                        const float rvx = pj->vx - pi->vx;
                        const float rvy = pj->vy - pi->vy;
                        const float vn = rvx * nx + rvy * ny;
                        if (vn < 0.0f) {
                            const float impulse = -vn * 0.18f;
                            pi->vx -= nx * impulse;
                            pi->vy -= ny * impulse;
                            pj->vx += nx * impulse;
                            pj->vy += ny * impulse;
                        }
                    }
                }
            }
        }

        for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
            fluid_apply_boundary(&s_particles[i]);
            fluid_limit_velocity(&s_particles[i]);
        }
    }
}

static float fluid_pressure_from_density(const float density)
{

    const float ratio = density / FLUID_REST_DENSITY;
    const float ratio2 = ratio * ratio;
    const float ratio4 = ratio2 * ratio2;
    const float ratio7 = ratio4 * ratio2 * ratio;
    const float pressure = FLUID_STIFFNESS * (ratio7 - 1.0f);
    return fluid_clampf(pressure, 0.0f, FLUID_PRESSURE_CLAMP);
}

static void fluid_compute_density_pressure(void)
{
    for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
        fluid_particle_t *pi = &s_particles[i];
        float density = 0.0f;
        const int base_cell_x = (int)fluid_clampf(floorf(pi->x / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_W - 1));
        const int base_cell_y = (int)fluid_clampf(floorf(pi->y / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_H - 1));

        for (int offset_y = -1; offset_y <= 1; ++offset_y) {
            const int cell_y = base_cell_y + offset_y;
            if (cell_y < 0 || cell_y >= FLUID_GRID_H) {
                continue;
            }

            for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                const int cell_x = base_cell_x + offset_x;
                if (cell_x < 0 || cell_x >= FLUID_GRID_W) {
                    continue;
                }

                for (int16_t j = s_cell_head[fluid_cell_index(cell_x, cell_y)]; j >= 0; j = s_particles[j].next) {
                    const float dx = pi->x - s_particles[j].x;
                    const float dy = pi->y - s_particles[j].y;
                    const float r_sq = dx * dx + dy * dy;
                    density += FLUID_PARTICLE_MASS * fluid_poly6_kernel(r_sq);
                }
            }
        }

        pi->density = fmaxf(density, FLUID_REST_DENSITY * 0.65f);
        pi->pressure = fluid_pressure_from_density(pi->density);
    }
}

static void fluid_compute_forces(const float accel_x_g, const float accel_y_g)
{
    const float gravity_x = fluid_clampf(accel_x_g, -FLUID_MAX_ACCEL_G, FLUID_MAX_ACCEL_G) * FLUID_GRAVITY_SCALE;
    const float gravity_y = fluid_clampf(accel_y_g, -FLUID_MAX_ACCEL_G, FLUID_MAX_ACCEL_G) * FLUID_GRAVITY_SCALE;

    for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
        fluid_particle_t *pi = &s_particles[i];
        float ax = gravity_x;
        float ay = gravity_y;
        float xsph_x = 0.0f;
        float xsph_y = 0.0f;
        const int base_cell_x = (int)fluid_clampf(floorf(pi->x / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_W - 1));
        const int base_cell_y = (int)fluid_clampf(floorf(pi->y / FLUID_CELL_SIZE), 0.0f, (float)(FLUID_GRID_H - 1));

        for (int offset_y = -1; offset_y <= 1; ++offset_y) {
            const int cell_y = base_cell_y + offset_y;
            if (cell_y < 0 || cell_y >= FLUID_GRID_H) {
                continue;
            }

            for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                const int cell_x = base_cell_x + offset_x;
                if (cell_x < 0 || cell_x >= FLUID_GRID_W) {
                    continue;
                }

                for (int16_t j = s_cell_head[fluid_cell_index(cell_x, cell_y)]; j >= 0; j = s_particles[j].next) {
                    if (j == i) {
                        continue;
                    }

                    fluid_particle_t *pj = &s_particles[j];
                    const float dx = pi->x - pj->x;
                    const float dy = pi->y - pj->y;
                    const float r_sq = dx * dx + dy * dy;
                    if (r_sq >= FLUID_SUPPORT_RADIUS_SQ || r_sq <= 0.0001f) {
                        continue;
                    }

                    const float r = sqrtf(r_sq);
                    const float inv_r = 1.0f / r;
                    const float pressure_scale = FLUID_PARTICLE_MASS *
                        (pi->pressure / (pi->density * pi->density) + pj->pressure / (pj->density * pj->density)) *
                        fluid_spiky_grad_kernel(r) * inv_r;
                    ax -= pressure_scale * dx;
                    ay -= pressure_scale * dy;

                    const float viscosity_scale = FLUID_VISCOSITY * FLUID_PARTICLE_MASS *
                        fluid_visc_laplacian_kernel(r) / pj->density;
                    ax += viscosity_scale * (pj->vx - pi->vx);
                    ay += viscosity_scale * (pj->vy - pi->vy);

                    const float xsph_weight = FLUID_PARTICLE_MASS * fluid_poly6_kernel(r_sq) / pj->density;
                    xsph_x += (pj->vx - pi->vx) * xsph_weight;
                    xsph_y += (pj->vy - pi->vy) * xsph_weight;
                }
            }
        }

        const float accel_sq = ax * ax + ay * ay;
        if (accel_sq > FLUID_MAX_FORCE_ACCEL * FLUID_MAX_FORCE_ACCEL) {
            const float accel_scale = FLUID_MAX_FORCE_ACCEL / sqrtf(accel_sq);
            ax *= accel_scale;
            ay *= accel_scale;
        }

        pi->ax = ax;
        pi->ay = ay;
        pi->xsph_x = xsph_x;
        pi->xsph_y = xsph_y;
    }
}

static void fluid_integrate(const float dt)
{
    for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
        fluid_particle_t *particle = &s_particles[i];
        particle->vx += particle->ax * dt;
        particle->vy += particle->ay * dt;
        particle->vx += particle->xsph_x * FLUID_XSPH_C;
        particle->vy += particle->xsph_y * FLUID_XSPH_C;
        particle->vx *= FLUID_VELOCITY_DAMPING;
        particle->vy *= FLUID_VELOCITY_DAMPING;
        fluid_limit_velocity(particle);
        particle->x += particle->vx * dt;
        particle->y += particle->vy * dt;
        fluid_apply_boundary(particle);
    }

    fluid_relax_particles();
}


void fluid_sim_init(void)
{
    const float start_x = 32.0f;
    const float start_y = 53.0f;
    const float spacing_x = 10.5f;
    const float spacing_y = 9.5f;
    const float center = FLUID_SIM_CANVAS_SIZE * 0.5f;
    const float limit = FLUID_CONTAINER_RADIUS - 9.0f;
    int index = 0;

    memset(s_particles, 0, sizeof(s_particles));
    memset(s_cell_head, 0xff, sizeof(s_cell_head));

    for (int row = 0; row < 8 && index < FLUID_PARTICLE_COUNT; ++row) {
        for (int col = 0; col < 8 && index < FLUID_PARTICLE_COUNT; ++col) {
            const float offset_x = (row & 1) ? (spacing_x * 0.5f) : 0.0f;
            const float x = start_x + col * spacing_x + offset_x;
            const float y = start_y + row * spacing_y;
            const float dx = x - center;
            const float dy = y - center;

            if (dx * dx + dy * dy > limit * limit) {
                continue;
            }

            s_particles[index].x = x;
            s_particles[index].y = y;
            ++index;
        }
    }

    while (index < FLUID_PARTICLE_COUNT) {
        const float angle = ((float)index / (float)FLUID_PARTICLE_COUNT) * 2.0f * M_PI;
        s_particles[index].x = center + cosf(angle) * 14.0f;
        s_particles[index].y = center + sinf(angle) * 12.0f;
        ++index;
    }

    s_initialized = true;
}

void fluid_sim_step(const float accel_x_g, const float accel_y_g, uint32_t dt_ms)
{
    if (!s_initialized) {
        fluid_sim_init();
    }

    if (dt_ms == 0) {
        dt_ms = 16;
    }

    const uint32_t clamped_dt_ms = dt_ms > 36 ? 36 : dt_ms;
    int substeps = (int)((clamped_dt_ms + 11U) / 12U);
    if (substeps < 1) {
        substeps = 1;
    } else if (substeps > 3) {
        substeps = 3;
    }
    const float dt = (float)clamped_dt_ms / 1000.0f / (float)substeps;


    for (int step = 0; step < substeps; ++step) {
        fluid_build_grid();
        fluid_compute_density_pressure();
        fluid_compute_forces(accel_x_g, accel_y_g);
        fluid_integrate(dt);
    }

    /* 诊断日志：每秒输出一次颗粒平均速度和输入重力 */
    static uint32_t s_fluid_log_tick = 0;
    if (++s_fluid_log_tick >= 30) {
        s_fluid_log_tick = 0;
        float avg_spd = 0.0f;
        float max_spd = 0.0f;
        for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
            float spd = sqrtf(s_particles[i].vx * s_particles[i].vx +
                              s_particles[i].vy * s_particles[i].vy);
            avg_spd += spd;
            if (spd > max_spd) max_spd = spd;
        }
        avg_spd /= FLUID_PARTICLE_COUNT;
        float gx = fluid_clampf(accel_x_g, -FLUID_MAX_ACCEL_G, FLUID_MAX_ACCEL_G) * FLUID_GRAVITY_SCALE;
        float gy = fluid_clampf(accel_y_g, -FLUID_MAX_ACCEL_G, FLUID_MAX_ACCEL_G) * FLUID_GRAVITY_SCALE;
        ESP_LOGI(TAG_FLUID, "[FLUID] in=(%.3f,%.3f) grav=(%.1f,%.1f) avg_spd=%.1f max_spd=%.1f",
                 accel_x_g, accel_y_g, gx, gy, avg_spd, max_spd);
    }
}

void fluid_sim_render(lv_obj_t *canvas)
{
    if (!s_initialized || canvas == NULL) {
        return;
    }

    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t vessel_dsc;
    lv_draw_rect_dsc_init(&vessel_dsc);
    vessel_dsc.radius = LV_RADIUS_CIRCLE;
    vessel_dsc.bg_color = lv_color_make(8, 18, 30);
    vessel_dsc.bg_opa = LV_OPA_40;
    vessel_dsc.border_color = lv_color_make(70, 120, 170);
    vessel_dsc.border_width = 2;
    vessel_dsc.border_opa = LV_OPA_60;
    lv_area_t vessel_area = {1, 1, FLUID_SIM_CANVAS_SIZE - 2, FLUID_SIM_CANVAS_SIZE - 2};
    lv_draw_rect(&layer, &vessel_dsc, &vessel_area);

    lv_draw_rect_dsc_t highlight_dsc;
    lv_draw_rect_dsc_init(&highlight_dsc);
    highlight_dsc.radius = LV_RADIUS_CIRCLE;
    highlight_dsc.bg_color = lv_color_make(120, 180, 255);
    highlight_dsc.bg_opa = 28;

    lv_area_t highlight_area = {16, 10, FLUID_SIM_CANVAS_SIZE - 28, FLUID_SIM_CANVAS_SIZE / 2};
    lv_draw_rect(&layer, &highlight_dsc, &highlight_area);

    for (int i = 0; i < FLUID_PARTICLE_COUNT; ++i) {
        const fluid_particle_t *particle = &s_particles[i];
        const float speed = sqrtf(particle->vx * particle->vx + particle->vy * particle->vy);
        const float density_ratio = fluid_clampf((particle->density / FLUID_REST_DENSITY) - 0.8f, 0.0f, 1.2f);
        const uint8_t red = (uint8_t)(18.0f + fminf(speed * 0.08f, 50.0f));
        const uint8_t green = (uint8_t)(140.0f + density_ratio * 65.0f);
        const uint8_t blue = (uint8_t)(225.0f + density_ratio * 24.0f);

        lv_draw_rect_dsc_t glow_dsc;
        lv_draw_rect_dsc_init(&glow_dsc);
        glow_dsc.radius = LV_RADIUS_CIRCLE;
        glow_dsc.bg_color = lv_color_make(red, green, blue);
        glow_dsc.bg_opa = 20;
        const int16_t glow_r = FLUID_PARTICLE_DRAW_RADIUS + 1;

        lv_area_t glow_area = {
            (int32_t)(particle->x - glow_r),
            (int32_t)(particle->y - glow_r),
            (int32_t)(particle->x + glow_r),
            (int32_t)(particle->y + glow_r)
        };
        lv_draw_rect(&layer, &glow_dsc, &glow_area);

        lv_draw_rect_dsc_t core_dsc;
        lv_draw_rect_dsc_init(&core_dsc);
        core_dsc.radius = LV_RADIUS_CIRCLE;
        core_dsc.bg_color = lv_color_make(red, green, blue);
        core_dsc.bg_opa = LV_OPA_80;
        core_dsc.border_color = lv_color_make(210, 240, 255);
        core_dsc.border_width = 1;
        core_dsc.border_opa = 115;

        const int16_t core_r = FLUID_PARTICLE_DRAW_RADIUS;
        lv_area_t core_area = {
            (int32_t)(particle->x - core_r),
            (int32_t)(particle->y - core_r),
            (int32_t)(particle->x + core_r),
            (int32_t)(particle->y + core_r)
        };
        lv_draw_rect(&layer, &core_dsc, &core_area);
    }

    lv_canvas_finish_layer(canvas, &layer);
}
