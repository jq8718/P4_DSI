#include "p4scan_lcm_display.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCM_DSI_LANES P4SCAN_LCM_DSI_LANES
#define LCM_DSI_LANE_RATE_MBPS P4SCAN_LCM_DSI_LANE_RATE_MBPS
#define LCM_DPI_CLOCK_MHZ P4SCAN_LCM_DPI_CLOCK_MHZ
#define LCM_PIXEL_BITS_PER_PIXEL P4SCAN_LCM_PIXEL_BITS

// ILI9882Q page 6, register D9: the panel pad setting is independent of the
// number of lanes configured in the ESP32-P4 DSI host.
#define ILI9882Q_DSI_2_LANE_PAD 0x0F
#define ILI9882Q_DSI_4_LANE_PAD 0x1F

#if LCM_DSI_LANES != 2
#error "This ESP32-P4 target has a 2-lane DSI connection"
#endif

#define ILI9882Q_DSI_LANE_PAD ILI9882Q_DSI_2_LANE_PAD

#define LCM_HBP P4SCAN_LCM_HBP
#define LCM_HSYNC P4SCAN_LCM_HSYNC
#define LCM_HFP P4SCAN_LCM_HFP
#define LCM_VBP P4SCAN_LCM_VBP
#define LCM_VSYNC P4SCAN_LCM_VSYNC
#define LCM_VFP P4SCAN_LCM_VFP
#define LCM_H_TOTAL P4SCAN_LCM_H_TOTAL
#define LCM_V_TOTAL P4SCAN_LCM_V_TOTAL

#define LCM_BACKLIGHT_GPIO 22
#define LCM_BACKLIGHT_FREQ_HZ 1000
#define LCM_BACKLIGHT_TIMER LEDC_TIMER_0
#define LCM_BACKLIGHT_CHANNEL LEDC_CHANNEL_0
#define LCM_BACKLIGHT_MODE LEDC_LOW_SPEED_MODE
#define LCM_BACKLIGHT_RESOLUTION LEDC_TIMER_13_BIT
#define LCM_BACKLIGHT_MAX_DUTY ((1U << 13) - 1U)

#define LCM_DSI_PHY_LDO_CHANNEL 3
#define LCM_DSI_PHY_LDO_VOLTAGE_MV 2500

static const char *TAG = "p4scan_lcm_display";
static bool s_backlight_ready;
static esp_ldo_channel_handle_t s_dsi_phy_ldo;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint16_t delay_ms;
    uint8_t data[40];
} ili9882q_cmd_t;

#define ILI_CMD(c, n, ...) { (c), (n), 0, { __VA_ARGS__ } }
#define ILI_DELAY(ms) { 0, 0, (ms), { 0 } }

// Converted from xc_ili9882q_dsi_vdo_hdplus_lc_t2351.c.txt.
static const ili9882q_cmd_t s_ili9882q_init[] = {
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x01),
    ILI_CMD(0x00, 1, 0x4A), ILI_CMD(0x01, 1, 0x38), ILI_CMD(0x02, 1, 0x00),
    ILI_CMD(0x03, 1, 0x00), ILI_CMD(0x04, 1, 0xC3), ILI_CMD(0x05, 1, 0x15),
    ILI_CMD(0x06, 1, 0x00), ILI_CMD(0x07, 1, 0x00), ILI_CMD(0x24, 1, 0x81),
    ILI_CMD(0x25, 1, 0x0D), ILI_CMD(0x26, 1, 0x00), ILI_CMD(0x27, 1, 0x00),
    ILI_CMD(0x08, 1, 0x84), ILI_CMD(0x09, 1, 0x85), ILI_CMD(0x0A, 1, 0xF5),
    ILI_CMD(0x0C, 1, 0x00), ILI_CMD(0x0D, 1, 0x00), ILI_CMD(0x0E, 1, 0x00),
    ILI_CMD(0x0F, 1, 0x00), ILI_CMD(0x0B, 1, 0x00), ILI_CMD(0x16, 1, 0x84),
    ILI_CMD(0x17, 1, 0x85), ILI_CMD(0x18, 1, 0x75), ILI_CMD(0x1A, 1, 0x00),
    ILI_CMD(0x1B, 1, 0x00), ILI_CMD(0x1C, 1, 0x00), ILI_CMD(0x1D, 1, 0x00),
    ILI_CMD(0x19, 1, 0x00),
    ILI_CMD(0x31, 1, 0x07), ILI_CMD(0x32, 1, 0x02), ILI_CMD(0x33, 1, 0x21),
    ILI_CMD(0x34, 1, 0x0D), ILI_CMD(0x35, 1, 0x00), ILI_CMD(0x36, 1, 0x01),
    ILI_CMD(0x37, 1, 0x1F), ILI_CMD(0x38, 1, 0x1D), ILI_CMD(0x39, 1, 0x1B),
    ILI_CMD(0x3A, 1, 0x19), ILI_CMD(0x3B, 1, 0x17), ILI_CMD(0x3C, 1, 0x15),
    ILI_CMD(0x3D, 1, 0x13), ILI_CMD(0x3E, 1, 0x11), ILI_CMD(0x3F, 1, 0x09),
    ILI_CMD(0x40, 1, 0x0B), ILI_CMD(0x41, 1, 0x07), ILI_CMD(0x42, 1, 0x02),
    ILI_CMD(0x43, 1, 0x07), ILI_CMD(0x44, 1, 0x07), ILI_CMD(0x45, 1, 0x07),
    ILI_CMD(0x46, 1, 0x07),
    ILI_CMD(0x47, 1, 0x07), ILI_CMD(0x48, 1, 0x02), ILI_CMD(0x49, 1, 0x20),
    ILI_CMD(0x4A, 1, 0x0C), ILI_CMD(0x4B, 1, 0x00), ILI_CMD(0x4C, 1, 0x01),
    ILI_CMD(0x4D, 1, 0x1E), ILI_CMD(0x4E, 1, 0x1C), ILI_CMD(0x4F, 1, 0x1A),
    ILI_CMD(0x50, 1, 0x18), ILI_CMD(0x51, 1, 0x16), ILI_CMD(0x52, 1, 0x14),
    ILI_CMD(0x53, 1, 0x12), ILI_CMD(0x54, 1, 0x10), ILI_CMD(0x55, 1, 0x08),
    ILI_CMD(0x56, 1, 0x0A), ILI_CMD(0x57, 1, 0x07), ILI_CMD(0x58, 1, 0x02),
    ILI_CMD(0x59, 1, 0x07), ILI_CMD(0x5A, 1, 0x07), ILI_CMD(0x5B, 1, 0x07),
    ILI_CMD(0x5C, 1, 0x07),
    ILI_CMD(0x61, 1, 0x07), ILI_CMD(0x62, 1, 0x02), ILI_CMD(0x63, 1, 0x08),
    ILI_CMD(0x64, 1, 0x0A), ILI_CMD(0x65, 1, 0x00), ILI_CMD(0x66, 1, 0x01),
    ILI_CMD(0x67, 1, 0x10), ILI_CMD(0x68, 1, 0x12), ILI_CMD(0x69, 1, 0x14),
    ILI_CMD(0x6A, 1, 0x16), ILI_CMD(0x6B, 1, 0x18), ILI_CMD(0x6C, 1, 0x1A),
    ILI_CMD(0x6D, 1, 0x1C), ILI_CMD(0x6E, 1, 0x1E), ILI_CMD(0x6F, 1, 0x20),
    ILI_CMD(0x70, 1, 0x0C), ILI_CMD(0x71, 1, 0x07), ILI_CMD(0x72, 1, 0x02),
    ILI_CMD(0x73, 1, 0x07), ILI_CMD(0x74, 1, 0x07), ILI_CMD(0x75, 1, 0x07),
    ILI_CMD(0x76, 1, 0x07),
    ILI_CMD(0x77, 1, 0x07), ILI_CMD(0x78, 1, 0x02), ILI_CMD(0x79, 1, 0x09),
    ILI_CMD(0x7A, 1, 0x0B), ILI_CMD(0x7B, 1, 0x00), ILI_CMD(0x7C, 1, 0x01),
    ILI_CMD(0x7D, 1, 0x11), ILI_CMD(0x7E, 1, 0x13), ILI_CMD(0x7F, 1, 0x15),
    ILI_CMD(0x80, 1, 0x17), ILI_CMD(0x81, 1, 0x19), ILI_CMD(0x82, 1, 0x1B),
    ILI_CMD(0x83, 1, 0x1D), ILI_CMD(0x84, 1, 0x1F), ILI_CMD(0x85, 1, 0x21),
    ILI_CMD(0x86, 1, 0x0D), ILI_CMD(0x87, 1, 0x07), ILI_CMD(0x88, 1, 0x02),
    ILI_CMD(0x89, 1, 0x07), ILI_CMD(0x8A, 1, 0x07), ILI_CMD(0x8B, 1, 0x07),
    ILI_CMD(0x8C, 1, 0x07),
    ILI_CMD(0xD1, 1, 0x00), ILI_CMD(0xE2, 1, 0x00), ILI_CMD(0xE6, 1, 0x22),
    ILI_CMD(0xE7, 1, 0x54),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x02), ILI_CMD(0xF1, 1, 0x1C),
    ILI_CMD(0x4B, 1, 0x5A), ILI_CMD(0x50, 1, 0xCA), ILI_CMD(0x51, 1, 0x00),
    ILI_CMD(0x06, 1, 0x9D), ILI_CMD(0x0B, 1, 0xA0), ILI_CMD(0x0C, 1, 0x00),
    ILI_CMD(0x0D, 1, 0x14), ILI_CMD(0x0E, 1, 0xDC), ILI_CMD(0x4E, 1, 0x11),
    ILI_CMD(0x4D, 1, 0xCE), ILI_CMD(0xF2, 1, 0x4A), ILI_CMD(0x40, 1, 0x49),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x05), ILI_CMD(0x03, 1, 0x00),
    ILI_CMD(0x04, 1, 0xD7), ILI_CMD(0x63, 1, 0x7E), ILI_CMD(0x64, 1, 0x7E),
    ILI_CMD(0x68, 1, 0xA1), ILI_CMD(0x69, 1, 0xA7), ILI_CMD(0x6A, 1, 0xA1),
    ILI_CMD(0x6B, 1, 0x93), ILI_CMD(0x85, 1, 0x37), ILI_CMD(0x23, 1, 0x42),
    ILI_CMD(0x24, 1, 0x52),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x06),
    // ILI9882Q page 6: the supplied 4-lane table uses 0x1F. The ESP32-P4
    // board is wired for two lanes, so use the matching panel pad setting.
    ILI_CMD(0xD9, 1, ILI9882Q_DSI_LANE_PAD),
    ILI_CMD(0xC0, 1, 0xA0), ILI_CMD(0xC1, 1, 0x15), ILI_CMD(0x80, 1, 0x09),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x08),
    ILI_CMD(0xE0, 0x27, 0x00, 0x00, 0x63, 0x9C, 0xDF, 0x55, 0x13, 0x3A,
            0x68, 0x8C, 0xA5, 0xC5, 0xF1, 0x19, 0x3F, 0xAA, 0x68, 0x9A,
            0xBA, 0xE4, 0xFF, 0x07, 0x36, 0x6E, 0x9E, 0x03, 0xEC),
    ILI_CMD(0xE1, 0x27, 0x00, 0x00, 0x63, 0x9C, 0xDF, 0x55, 0x13, 0x3A,
            0x68, 0x8C, 0xA5, 0xC5, 0xF1, 0x19, 0x3F, 0xAA, 0x68, 0x9A,
            0xBA, 0xE4, 0xFF, 0x07, 0x36, 0x6E, 0x9E, 0x03, 0xEC),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x0B), ILI_CMD(0x9A, 1, 0x89),
    ILI_CMD(0x9B, 1, 0xEA), ILI_CMD(0x9C, 1, 0x07), ILI_CMD(0x9D, 1, 0x07),
    ILI_CMD(0x9E, 1, 0xF7), ILI_CMD(0x9F, 1, 0xF7), ILI_CMD(0xAA, 1, 0x22),
    ILI_CMD(0xAB, 1, 0xE0), ILI_CMD(0xAC, 1, 0x7F), ILI_CMD(0xAD, 1, 0x3F),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x0E), ILI_CMD(0x11, 1, 0x10),
    ILI_CMD(0x12, 1, 0x08), ILI_CMD(0x13, 1, 0x14), ILI_CMD(0x00, 1, 0xA0),
    ILI_CMD(0xFF, 3, 0x98, 0x82, 0x00), ILI_CMD(0x35, 1, 0x00),
    ILI_CMD(0x3A, 1, P4SCAN_LCM_PIXEL_FORMAT_DCS),
    // Keep the vendor table's one-byte payload for Sleep Out and Display On.
    // The ILI9882Q reference sequence uses 0x00 for both payload bytes.
    ILI_CMD(0x11, 1, 0x00), ILI_DELAY(120), ILI_CMD(0x29, 1, 0x00), ILI_DELAY(20),
};

static esp_err_t ili9882q_send_init(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_ili9882q_init) / sizeof(s_ili9882q_init[0]); ++i) {
        const ili9882q_cmd_t *entry = &s_ili9882q_init[i];
        if (entry->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(entry->delay_ms));
            continue;
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, entry->cmd, entry->data, entry->len),
                            TAG, "ILI9882Q command 0x%02x failed", entry->cmd);
    }
    return ESP_OK;
}

esp_err_t p4scan_lcm_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LCM_BACKLIGHT_MODE,
        .duty_resolution = LCM_BACKLIGHT_RESOLUTION,
        .timer_num = LCM_BACKLIGHT_TIMER,
        .freq_hz = LCM_BACKLIGHT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = LCM_BACKLIGHT_GPIO,
        .speed_mode = LCM_BACKLIGHT_MODE,
        .channel = LCM_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCM_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "failed to configure backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "failed to configure backlight GPIO");
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t p4scan_lcm_backlight_set_brightness(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_backlight_ready, ESP_ERR_INVALID_STATE, TAG,
                        "backlight is not initialized");
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (LCM_BACKLIGHT_MAX_DUTY * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCM_BACKLIGHT_MODE, LCM_BACKLIGHT_CHANNEL, duty), TAG,
                        "failed to set backlight duty");
    return ledc_update_duty(LCM_BACKLIGHT_MODE, LCM_BACKLIGHT_CHANNEL);
}

esp_err_t p4scan_lcm_backlight_off(void)
{
    return p4scan_lcm_backlight_set_brightness(0);
}

esp_err_t p4scan_lcm_display_init(esp_lcd_panel_handle_t *ret_panel, void **ret_frame_buffer)
{
    ESP_RETURN_ON_FALSE(ret_panel && ret_frame_buffer, ESP_ERR_INVALID_ARG, TAG,
                        "output pointer is NULL");

    if (!s_dsi_phy_ldo) {
        const esp_ldo_channel_config_t ldo_config = {
            .chan_id = LCM_DSI_PHY_LDO_CHANNEL,
            .voltage_mv = LCM_DSI_PHY_LDO_VOLTAGE_MV,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &s_dsi_phy_ldo), TAG,
                            "failed to power DSI PHY");
        ESP_LOGI(TAG, "MIPI DSI PHY powered: LDO channel=%d voltage=%dmV",
                 LCM_DSI_PHY_LDO_CHANNEL, LCM_DSI_PHY_LDO_VOLTAGE_MV);
    }

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCM_DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCM_DSI_LANE_RATE_MBPS,
    };
    esp_lcd_dsi_bus_handle_t bus = NULL;
    esp_err_t ret = esp_lcd_new_dsi_bus(&bus_config, &bus);
    if (ret != ESP_OK) {
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
        return ret;
    }

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    ret = esp_lcd_new_panel_io_dbi(bus, &dbi_config, &dbi_io);
    if (ret != ESP_OK) {
        esp_lcd_del_dsi_bus(bus);
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
        return ret;
    }

    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        // The selected mode uses an integer PLL divider and stays below the
        // configured video bandwidth limit.
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_PLL_F240M,
        .dpi_clock_freq_mhz = LCM_DPI_CLOCK_MHZ,
        .pixel_format = P4SCAN_LCM_USE_RGB888 ? LCD_COLOR_PIXEL_FORMAT_RGB888 : LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format = P4SCAN_LCM_USE_RGB888 ? LCD_COLOR_FMT_RGB888 : LCD_COLOR_FMT_RGB565,
        .out_color_format = P4SCAN_LCM_USE_RGB888 ? LCD_COLOR_FMT_RGB888 : LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = P4SCAN_LCM_H_RES,
            .v_size = P4SCAN_LCM_V_RES,
            .hsync_back_porch = LCM_HBP,
            .hsync_pulse_width = LCM_HSYNC,
            .hsync_front_porch = LCM_HFP,
            .vsync_back_porch = LCM_VBP,
            .vsync_pulse_width = LCM_VSYNC,
            .vsync_front_porch = LCM_VFP,
        },
    };
    esp_lcd_panel_handle_t panel = NULL;
    ret = esp_lcd_new_panel_dpi(bus, &dpi_config, &panel);
    if (ret != ESP_OK) {
        esp_lcd_panel_io_del(dbi_io);
        esp_lcd_del_dsi_bus(bus);
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
        return ret;
    }

    // PCA9538A has already released LCM_RST in p4scan_lcm_power_on().
    // The ESP32-P4 DSI bus is created in command mode with the clock lane in
    // LP. Start the DPI engine first so the generic command FIFO is serviced
    // while the panel initialization table is transmitted.
    ESP_LOGI(TAG, "starting DPI stream (%dx%d %s, %d lane)",
             P4SCAN_LCM_H_RES, P4SCAN_LCM_V_RES, P4SCAN_LCM_PIXEL_FORMAT_NAME,
             LCM_DSI_LANES);
    ret = esp_lcd_panel_init(panel);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "sending ILI9882Q initialization");
        ret = ili9882q_send_init(dbi_io);
    }
    if (ret != ESP_OK) {
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(dbi_io);
        esp_lcd_del_dsi_bus(bus);
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
        return ret;
    }

    void *frame_buffer = NULL;
    ret = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &frame_buffer);
    if (ret != ESP_OK) {
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(dbi_io);
        esp_lcd_del_dsi_bus(bus);
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
        return ret;
    }

    uint8_t *pixels = (uint8_t *)frame_buffer;
    for (int y = 0; y < P4SCAN_LCM_V_RES; ++y) {
        for (int x = 0; x < P4SCAN_LCM_H_RES; ++x) {
            size_t offset = (y * P4SCAN_LCM_H_RES + x) * P4SCAN_LCM_PIXEL_BYTES;
#if P4SCAN_LCM_USE_RGB888
            pixels[offset + 0] = 0x08;
            pixels[offset + 1] = 0x10;
            pixels[offset + 2] = 0x20;
#else
            ((uint16_t *)pixels)[y * P4SCAN_LCM_H_RES + x] = 0x0841;
#endif
        }
    }
    ret = esp_lcd_panel_draw_bitmap(panel, 0, 0, P4SCAN_LCM_H_RES, P4SCAN_LCM_V_RES,
                                    frame_buffer);
    if (ret != ESP_OK) {
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(dbi_io);
        esp_lcd_del_dsi_bus(bus);
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "%s dark background framebuffer submitted",
             P4SCAN_LCM_PIXEL_FORMAT_NAME);

    *ret_panel = panel;
    *ret_frame_buffer = frame_buffer;
    const uint32_t refresh_millihz = (uint32_t)(((uint64_t)LCM_DPI_CLOCK_MHZ * 1000000ULL * 1000ULL) /
                                                ((uint64_t)LCM_H_TOTAL * LCM_V_TOTAL));
    ESP_LOGI(TAG, "DPI panel started: htotal=%d vtotal=%d dpi=%dMHz refresh=%" PRIu32 ".%03" PRIu32 "Hz lane=%dMbps video_bw=%dMbps",
             LCM_H_TOTAL, LCM_V_TOTAL, LCM_DPI_CLOCK_MHZ,
             refresh_millihz / 1000, refresh_millihz % 1000, LCM_DSI_LANE_RATE_MBPS,
             LCM_DPI_CLOCK_MHZ * LCM_PIXEL_BITS_PER_PIXEL);
    return ESP_OK;
}
