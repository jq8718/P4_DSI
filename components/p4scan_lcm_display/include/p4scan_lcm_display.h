#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4SCAN_LCM_H_RES 720
#define P4SCAN_LCM_V_RES 1440

// Select the panel test mode in one place. The RGB888 setting is kept as a
// reproducible 1.5 Gbps-limited experiment; production defaults to RGB565.
#define P4SCAN_LCM_USE_RGB888 0
#define P4SCAN_LCM_DSI_LANES 2
#define P4SCAN_LCM_DSI_LANE_RATE_MBPS 1000
#define P4SCAN_LCM_MAX_VIDEO_BANDWIDTH_MBPS 1500

#if P4SCAN_LCM_USE_RGB888
#define P4SCAN_LCM_PIXEL_BYTES 3
#define P4SCAN_LCM_PIXEL_BITS 24
#define P4SCAN_LCM_PIXEL_FORMAT_DCS 0x77
#define P4SCAN_LCM_PIXEL_FORMAT_NAME "RGB888"
#define P4SCAN_LCM_DPI_CLOCK_MHZ 60
#define P4SCAN_LCM_HBP 14
#define P4SCAN_LCM_HSYNC 52
#define P4SCAN_LCM_HFP 14
#else
#define P4SCAN_LCM_PIXEL_BYTES 2
#define P4SCAN_LCM_PIXEL_BITS 16
#define P4SCAN_LCM_PIXEL_FORMAT_DCS 0x55
#define P4SCAN_LCM_PIXEL_FORMAT_NAME "RGB565"
#define P4SCAN_LCM_DPI_CLOCK_MHZ 80
#define P4SCAN_LCM_HBP 64
#define P4SCAN_LCM_HSYNC 52
#define P4SCAN_LCM_HFP 64
#endif

#define P4SCAN_LCM_VBP 16
#define P4SCAN_LCM_VSYNC 4
#define P4SCAN_LCM_VFP 20
#define P4SCAN_LCM_H_TOTAL \
    (P4SCAN_LCM_H_RES + P4SCAN_LCM_HBP + P4SCAN_LCM_HSYNC + P4SCAN_LCM_HFP)
#define P4SCAN_LCM_V_TOTAL \
    (P4SCAN_LCM_V_RES + P4SCAN_LCM_VBP + P4SCAN_LCM_VSYNC + P4SCAN_LCM_VFP)

#if P4SCAN_LCM_DPI_CLOCK_MHZ * P4SCAN_LCM_PIXEL_BITS > P4SCAN_LCM_MAX_VIDEO_BANDWIDTH_MBPS
#error "LCM video bandwidth exceeds the configured 1.5Gbps limit"
#endif

esp_err_t p4scan_lcm_display_init(esp_lcd_panel_handle_t *ret_panel,
                                   void **ret_frame_buffer);

esp_err_t p4scan_lcm_backlight_init(void);
esp_err_t p4scan_lcm_backlight_set_brightness(uint8_t percent);
esp_err_t p4scan_lcm_backlight_off(void);

#ifdef __cplusplus
}
#endif
