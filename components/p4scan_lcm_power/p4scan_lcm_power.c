#include "p4scan_lcm_power.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PCA9538A_REG_OUTPUT 0x01
#define PCA9538A_REG_CONFIG 0x03

#define PCA_P0_RST_CTP BIT0
#define PCA_P1_LCM_RST BIT1
#define PCA_P2_EN_1V8 BIT2
#define PCA_P3_EN_VGP1 BIT3
#define PCA_P4_LED1 BIT4
#define PCA_P5_ENN BIT5
#define PCA_P6_ENP BIT6
#define PCA_P7_BL_EN BIT7

static const char *TAG = "p4scan_lcm_power";
static i2c_master_dev_handle_t s_pca;
static uint8_t s_output;

static esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 0; attempt < 3; ++attempt) {
        ret = i2c_master_transmit(dev, data, sizeof(data), 100);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "I2C register write 0x%02x=0x%02x failed: %s",
             reg, value, esp_err_to_name(ret));
    return ret;
}

static esp_err_t pca_set_output(uint8_t value)
{
    ESP_RETURN_ON_FALSE(s_pca, ESP_ERR_INVALID_STATE, TAG, "PCA9538A is not initialized");
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_pca, PCA9538A_REG_OUTPUT, value), TAG,
                        "failed to set PCA9538A output");
    s_output = value;
    return ESP_OK;
}

esp_err_t p4scan_lcm_power_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "I2C bus is NULL");

    const i2c_device_config_t pca_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = P4SCAN_LCM_PCA9538A_ADDR,
        .scl_speed_hz = P4SCAN_LCM_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &pca_config, &s_pca), TAG,
                        "failed to add PCA9538A");

    // Set the latch before changing the PCA9538A pins from inputs to outputs.
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_pca, PCA9538A_REG_OUTPUT, 0x00), TAG,
                        "failed to clear PCA9538A output latch");
    ESP_RETURN_ON_ERROR(i2c_write_reg(s_pca, PCA9538A_REG_CONFIG, 0x00), TAG,
                        "failed to configure PCA9538A outputs");
    s_output = 0;

    ESP_LOGI(TAG, "LCM power controller ready: PCA9538A=0x%02x; TPS65132 uses default settings",
             P4SCAN_LCM_PCA9538A_ADDR);
    return ESP_OK;
}

esp_err_t p4scan_lcm_power_on(void)
{
    ESP_RETURN_ON_FALSE(s_pca, ESP_ERR_INVALID_STATE, TAG,
                        "LCM power is not initialized");

    ESP_RETURN_ON_ERROR(pca_set_output(0x00), TAG, "failed to assert initial reset state");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8), TAG, "failed to enable 1V8");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8 | PCA_P3_EN_VGP1), TAG,
                        "failed to enable VGP1");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8 | PCA_P3_EN_VGP1 | PCA_P5_ENN), TAG,
                        "failed to enable negative bias");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8 | PCA_P3_EN_VGP1 | PCA_P5_ENN | PCA_P6_ENP),
                        TAG, "failed to enable positive bias");
    vTaskDelay(pdMS_TO_TICKS(50));
    // The board uses TPS65132 default voltages. It is enabled only by ENN and
    // ENP; do not probe, register, or write its I2C address from this driver.
    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8 | PCA_P3_EN_VGP1 | PCA_P5_ENN |
                                       PCA_P6_ENP | PCA_P0_RST_CTP),
                        TAG, "failed to release touch reset");
    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8 | PCA_P3_EN_VGP1 | PCA_P5_ENN |
                                       PCA_P6_ENP | PCA_P0_RST_CTP | PCA_P1_LCM_RST),
                        TAG, "failed to release LCD reset");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(pca_set_output(PCA_P2_EN_1V8 | PCA_P3_EN_VGP1 | PCA_P4_LED1 |
                                       PCA_P5_ENN | PCA_P6_ENP | PCA_P0_RST_CTP | PCA_P1_LCM_RST),
                        TAG, "failed to enable LED1");

    ESP_LOGI(TAG, "LCM power-on sequence complete, output latch=0x%02x", s_output);
    return ESP_OK;
}

esp_err_t p4scan_lcm_set_reset_ctp(bool level)
{
    uint8_t output = level ? (s_output | PCA_P0_RST_CTP) : (s_output & ~PCA_P0_RST_CTP);
    return pca_set_output(output);
}

esp_err_t p4scan_lcm_set_backlight_enable(bool enable)
{
    uint8_t output = enable ? (s_output | PCA_P7_BL_EN) : (s_output & ~PCA_P7_BL_EN);
    return pca_set_output(output);
}

esp_err_t p4scan_lcm_power_off(void)
{
    ESP_RETURN_ON_FALSE(s_pca, ESP_ERR_INVALID_STATE, TAG, "LCM power is not initialized");
    ESP_RETURN_ON_ERROR(pca_set_output(s_output & ~PCA_P7_BL_EN), TAG,
                        "failed to disable backlight enable");
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_RETURN_ON_ERROR(pca_set_output(s_output & ~(PCA_P0_RST_CTP | PCA_P1_LCM_RST |
                                                    PCA_P6_ENP | PCA_P5_ENN | PCA_P3_EN_VGP1 |
                                                    PCA_P2_EN_1V8 | PCA_P4_LED1)),
                        TAG, "failed to disable LCM rails");
    ESP_LOGI(TAG, "LCM power-off sequence complete");
    return ESP_OK;
}
