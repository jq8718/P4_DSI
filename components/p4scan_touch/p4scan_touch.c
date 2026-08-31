#include "p4scan_touch.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4scan_lcm_power.h"

#define T2351_REPORT_ID 0x5A
#define T2351_REPORT_LENGTH 43
#define T2351_MAX_TOUCHES 5
#define T2351_TOUCH_COMMAND_LENGTH 3
#define T2351_RAW_COORDINATE_MAX 2048
#define T2351_LCD_WIDTH 720
#define T2351_LCD_HEIGHT 1440

static const char *TAG = "p4scan_touch";
static i2c_master_dev_handle_t s_touch;
static TaskHandle_t s_touch_task;
static bool s_touch_irq_added;
static volatile uint32_t s_touch_irq_count;
static bool s_touch_was_active;

static void IRAM_ATTR touch_isr_handler(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    ++s_touch_irq_count;
    vTaskNotifyGiveFromISR((TaskHandle_t)arg, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static uint8_t t2351_checksum(const uint8_t *packet, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += packet[i];
    }
    return (uint8_t)(-sum);
}

static uint16_t t2351_map_coordinate(uint16_t value, uint16_t output_max)
{
    if (value >= T2351_RAW_COORDINATE_MAX) {
        return output_max - 1;
    }
    return (uint32_t)value * output_max / T2351_RAW_COORDINATE_MAX;
}

static void t2351_log_report(const uint8_t *packet)
{
    unsigned active_count = 0;

    for (unsigned i = 0; i < T2351_MAX_TOUCHES; ++i) {
        const size_t offset = 1 + 4 * i;
        if (packet[offset] == 0xFF && packet[offset + 1] == 0xFF &&
            packet[offset + 2] == 0xFF) {
            continue;
        }

        const uint16_t x = (uint16_t)(((packet[offset] & 0xF0) << 4) |
                                     packet[offset + 1]);
        const uint16_t y = (uint16_t)(((packet[offset] & 0x0F) << 8) |
                                     packet[offset + 2]);
        const uint8_t pressure = packet[offset + 3];
        const uint16_t lcd_x = t2351_map_coordinate(x, T2351_LCD_WIDTH);
        const uint16_t lcd_y = t2351_map_coordinate(y, T2351_LCD_HEIGHT);
        ESP_LOGI(TAG, "touch[%u]: raw=(%u,%u) lcd=(%u,%u) pressure=%u",
                 i, x, y, lcd_x, lcd_y, pressure);
        ++active_count;
    }

    if (active_count != 0) {
        s_touch_was_active = true;
    } else if (s_touch_was_active) {
        ESP_LOGI(TAG, "touch release");
        s_touch_was_active = false;
    }
}

static bool t2351_read_report(uint8_t *packet)
{
    esp_err_t ret = i2c_master_receive(s_touch, packet, T2351_REPORT_LENGTH, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "touch report read failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (packet[0] != T2351_REPORT_ID) {
        ESP_LOGW(TAG, "unexpected touch report id 0x%02x", packet[0]);
        return false;
    }

    const uint8_t expected = t2351_checksum(packet, T2351_REPORT_LENGTH - 1);
    if (packet[T2351_REPORT_LENGTH - 1] != expected) {
        ESP_LOGW(TAG, "touch checksum error: expected=0x%02x received=0x%02x",
                 expected, packet[T2351_REPORT_LENGTH - 1]);
        return false;
    }

    return true;
}

static void t2351_drain_startup_reports(void)
{
    uint8_t packet[T2351_REPORT_LENGTH] = {0};

    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const int level_before = gpio_get_level(P4SCAN_TOUCH_INT_GPIO);
        if (level_before != 0) {
            break;
        }

        const bool valid = t2351_read_report(packet);
        const uint8_t expected = t2351_checksum(packet, T2351_REPORT_LENGTH - 1);
        ESP_LOGI(TAG,
                 "startup report[%u]: int_before=%d int_after=%d id=0x%02x "
                 "data=%02x %02x %02x %02x %02x %02x %02x checksum=%s",
                 attempt, level_before, gpio_get_level(P4SCAN_TOUCH_INT_GPIO),
                 packet[0], packet[1], packet[2], packet[3], packet[4], packet[5],
                 packet[6], packet[7], valid && packet[T2351_REPORT_LENGTH - 1] == expected
                     ? "ok" : "bad");
        if (gpio_get_level(P4SCAN_TOUCH_INT_GPIO) != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGI(TAG, "T2351 startup drain complete: INT level=%d",
             gpio_get_level(P4SCAN_TOUCH_INT_GPIO));
}

static void touch_task(void *arg)
{
    (void)arg;
    uint8_t packet[T2351_REPORT_LENGTH];

    while (true) {
        // The controller uses a level-held-low INT. Normally the falling-edge
        // ISR wakes this task; the timeout also handles a line that was
        // already low before the ISR was attached.
        const BaseType_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        const int int_level = gpio_get_level(P4SCAN_TOUCH_INT_GPIO);
        if (notified == 0 && int_level != 0) {
            continue;
        }

        const uint32_t irq_count = s_touch_irq_count;
        if (notified != 0 && (irq_count <= 8 || (irq_count % 32) == 0)) {
            ESP_LOGI(TAG, "touch IRQ #%lu: INT level=%d",
                     (unsigned long)irq_count, gpio_get_level(P4SCAN_TOUCH_INT_GPIO));
        }
        if (notified == 0) {
            ESP_LOGW(TAG, "T2351 INT remains low without a new edge; polling report");
        }

        // Read once for this wakeup, then retry a few times if the controller
        // still holds INT low. A valid report should release the line.
        unsigned reads = 0;
        do {
            if (t2351_read_report(packet)) {
                t2351_log_report(packet);
            }
            ++reads;
            if (gpio_get_level(P4SCAN_TOUCH_INT_GPIO) != 0 || reads >= 4) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        } while (true);

        // Drain notifications already queued by a burst of INT edges.
        while (ulTaskNotifyTake(pdTRUE, 0) != 0) {
            if (t2351_read_report(packet)) {
                t2351_log_report(packet);
            }
        }
    }
}

esp_err_t p4scan_touch_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret = ESP_OK;
    bool isr_service_installed = false;

    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "I2C bus is NULL");
    ESP_RETURN_ON_FALSE(!s_touch && !s_touch_task, ESP_ERR_INVALID_STATE, TAG,
                        "T2351 is already initialized");

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = P4SCAN_TOUCH_I2C_ADDR,
        .scl_speed_hz = P4SCAN_LCM_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(bus, &config, &s_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to add T2351 device: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Keep the open-drain INT line pulled high throughout reset and mode
    // selection. Interrupt generation stays disabled until startup reports
    // have been drained below.
    const gpio_config_t int_gpio_config = {
        .pin_bit_mask = 1ULL << P4SCAN_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&int_gpio_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure T2351 INT GPIO: %s", esp_err_to_name(ret));
        goto fail;
    }

    // RST_CTP is on PCA9538A P0. Re-run the reset here so a late touch init
    // always starts the controller in demo mode.
    ret = p4scan_lcm_set_reset_ctp(false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to assert T2351 reset: %s", esp_err_to_name(ret));
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = p4scan_lcm_set_reset_ctp(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to release T2351 reset: %s", esp_err_to_name(ret));
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    // Select the 43-byte demo report before enabling the GPIO interrupt. The
    // mode-switch command can assert INT once during controller startup; that
    // event is not a touch report and must not wake the report task.
    const uint8_t command[T2351_TOUCH_COMMAND_LENGTH] = {0xF2, 0x00, 0x03};
    ret = i2c_master_transmit(s_touch, command, sizeof(command), 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to select T2351 demo report mode: %s", esp_err_to_name(ret));
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    t2351_drain_startup_reports();

    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        goto fail;
    }
    isr_service_installed = true;

    ret = gpio_set_intr_type(P4SCAN_TOUCH_INT_GPIO, GPIO_INTR_NEGEDGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable T2351 INT falling-edge interrupt: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    BaseType_t task_ok = xTaskCreate(touch_task, "p4scan_touch", 4096, NULL, 10,
                                     &s_touch_task);
    if (task_ok != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "failed to create touch task");
        goto fail;
    }

    ret = gpio_isr_handler_add(P4SCAN_TOUCH_INT_GPIO, touch_isr_handler, s_touch_task);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to register T2351 INT handler: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_touch_irq_added = true;

    ESP_LOGI(TAG, "T2351 ready: addr=0x%02x INT=GPIO%d report=%d bytes",
             P4SCAN_TOUCH_I2C_ADDR, P4SCAN_TOUCH_INT_GPIO, T2351_REPORT_LENGTH);
    ESP_LOGI(TAG, "T2351 INT idle level=%d, IRQ count=%lu",
             gpio_get_level(P4SCAN_TOUCH_INT_GPIO), (unsigned long)s_touch_irq_count);

    // If the controller still holds INT low, make the task process it now;
    // the 100 ms level check remains as a recovery path for later stuck-low
    // conditions.
    if (gpio_get_level(P4SCAN_TOUCH_INT_GPIO) == 0) {
        xTaskNotifyGive(s_touch_task);
    }
    return ESP_OK;

fail:
    if (s_touch_irq_added) {
        gpio_isr_handler_remove(P4SCAN_TOUCH_INT_GPIO);
        s_touch_irq_added = false;
    }
    if (s_touch_task) {
        vTaskDelete(s_touch_task);
        s_touch_task = NULL;
    }
    if (isr_service_installed) {
        gpio_uninstall_isr_service();
    }
    if (s_touch) {
        i2c_master_bus_rm_device(s_touch);
        s_touch = NULL;
    }
    return ret;
}
