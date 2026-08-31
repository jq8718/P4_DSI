#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4SCAN_LCM_H_RES 720
#define P4SCAN_LCM_V_RES 1440

esp_err_t p4scan_lcm_display_init(esp_lcd_panel_handle_t *ret_panel,
                                   void **ret_frame_buffer);

esp_err_t p4scan_lcm_backlight_init(void);
esp_err_t p4scan_lcm_backlight_set_brightness(uint8_t percent);
esp_err_t p4scan_lcm_backlight_off(void);

#ifdef __cplusplus
}
#endif
