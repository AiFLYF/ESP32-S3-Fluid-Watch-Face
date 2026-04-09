idf_component_register(SRCS "main.c" "watch_ui.c" "fluid_sim.c" "accel_input.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip esp_timer)
