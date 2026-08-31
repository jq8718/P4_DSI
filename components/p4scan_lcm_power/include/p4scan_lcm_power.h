#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4SCAN_LCM_I2C_SDA_GPIO 7
#define P4SCAN_LCM_I2C_SCL_GPIO 8
#define P4SCAN_LCM_I2C_FREQ_HZ 100000

#define P4SCAN_LCM_PCA9538A_ADDR 0x71
esp_err_t p4scan_lcm_power_init(i2c_master_bus_handle_t bus);
esp_err_t p4scan_lcm_power_on(void);
esp_err_t p4scan_lcm_power_off(void);
esp_err_t p4scan_lcm_set_reset_ctp(bool level);
esp_err_t p4scan_lcm_set_backlight_enable(bool enable);

#ifdef __cplusplus
}
#endif
