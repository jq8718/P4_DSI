#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P4SCAN_TOUCH_INT_GPIO 23
#define P4SCAN_TOUCH_I2C_ADDR 0x41

esp_err_t p4scan_touch_init(i2c_master_bus_handle_t bus);

#ifdef __cplusplus
}
#endif
