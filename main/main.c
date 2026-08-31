#include "driver/i2c_master.h"
#include <sys/param.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "p4scan_lcm_display.h"
#include "p4scan_lcm_power.h"
#include "p4scan_touch.h"

static const char *TAG = "p4scan_lcm_demo";

// Keep the first LCD stability test independent of the touch controller and
// its I2C/IRQ activity. Re-enable after the panel output is stable.
#define P4SCAN_DISPLAY_ONLY 1

#define P4SCAN_ANIMATION_FRAME_MS 17
#define P4SCAN_SNAKE_CELL_SIZE 45
#define P4SCAN_SNAKE_COLS (P4SCAN_LCM_H_RES / P4SCAN_SNAKE_CELL_SIZE)
#define P4SCAN_SNAKE_ROWS (P4SCAN_LCM_V_RES / P4SCAN_SNAKE_CELL_SIZE)
#define P4SCAN_SNAKE_LENGTH 12
#define P4SCAN_SNAKE_STEP_FRAMES 3

typedef struct {
    int col;
    int row;
} snake_cell_t;

static void snake_path_point(uint32_t step, int *col, int *row)
{
    const uint32_t path_length = P4SCAN_SNAKE_COLS * P4SCAN_SNAKE_ROWS;
    step %= path_length;
    *row = (int)(step / P4SCAN_SNAKE_COLS);
    int path_col = (int)(step % P4SCAN_SNAKE_COLS);
    *col = (*row & 1) ? (P4SCAN_SNAKE_COLS - 1 - path_col) : path_col;
}

static void draw_snake_cell(uint16_t *pixels, int col, int row, uint16_t color, int inset)
{
    const int x0 = col * P4SCAN_SNAKE_CELL_SIZE + inset;
    const int y0 = row * P4SCAN_SNAKE_CELL_SIZE + inset;
    const int x1 = (col + 1) * P4SCAN_SNAKE_CELL_SIZE - inset;
    const int y1 = (row + 1) * P4SCAN_SNAKE_CELL_SIZE - inset;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            pixels[y * P4SCAN_LCM_H_RES + x] = color;
        }
    }
}

static uint16_t snake_background_color(void)
{
    return 0x0841;
}

static void restore_checker_cell(uint16_t *pixels, int col, int row)
{
    const int x0 = col * P4SCAN_SNAKE_CELL_SIZE;
    const int y0 = row * P4SCAN_SNAKE_CELL_SIZE;
    const int x1 = x0 + P4SCAN_SNAKE_CELL_SIZE;
    const int y1 = y0 + P4SCAN_SNAKE_CELL_SIZE;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            pixels[y * P4SCAN_LCM_H_RES + x] = snake_background_color();
        }
    }
}

static snake_cell_t snake_cell_at(uint32_t head_step, int segment)
{
    const uint32_t path_length = P4SCAN_SNAKE_COLS * P4SCAN_SNAKE_ROWS;
    snake_cell_t cell;
    snake_path_point(head_step + path_length - (uint32_t)segment,
                     &cell.col, &cell.row);
    return cell;
}

static void draw_snake_state(uint16_t *pixels, uint32_t head_step, uint32_t food_step,
                             int *min_row, int *max_row)
{
    *min_row = P4SCAN_SNAKE_ROWS;
    *max_row = 0;

    // Draw the body from tail to head so every frame has a deterministic shape.
    for (int segment = P4SCAN_SNAKE_LENGTH - 1; segment >= 0; --segment) {
        snake_cell_t cell = snake_cell_at(head_step, segment);
        draw_snake_cell(pixels, cell.col, cell.row, segment == 0 ? 0xFFE0 : 0x07E0, 4);
        *min_row = MIN(*min_row, cell.row);
        *max_row = MAX(*max_row, cell.row);
    }

    snake_cell_t food;
    snake_path_point(food_step, &food.col, &food.row);
    draw_snake_cell(pixels, food.col, food.row, 0xF800, 9);
    *min_row = MIN(*min_row, food.row);
    *max_row = MAX(*max_row, food.row);
}

static void update_snake_frame(uint16_t *pixels, uint32_t old_head_step,
                               uint32_t head_step, uint32_t old_food_step,
                               uint32_t food_step, int *min_row, int *max_row)
{
    *min_row = P4SCAN_SNAKE_ROWS;
    *max_row = 0;

    snake_cell_t old_food;
    snake_path_point(old_food_step, &old_food.col, &old_food.row);
    restore_checker_cell(pixels, old_food.col, old_food.row);
    *min_row = MIN(*min_row, old_food.row);
    *max_row = MAX(*max_row, old_food.row);

    for (int segment = P4SCAN_SNAKE_LENGTH - 1; segment >= 0; --segment) {
        snake_cell_t old_cell = snake_cell_at(old_head_step, segment);
        restore_checker_cell(pixels, old_cell.col, old_cell.row);
        *min_row = MIN(*min_row, old_cell.row);
        *max_row = MAX(*max_row, old_cell.row);
    }

    int new_min_row;
    int new_max_row;
    draw_snake_state(pixels, head_step, food_step, &new_min_row, &new_max_row);
    *min_row = MIN(*min_row, new_min_row);
    *max_row = MAX(*max_row, new_max_row);
}

void app_main(void)
{
    i2c_master_bus_config_t i2c_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = P4SCAN_LCM_I2C_SDA_GPIO,
        .scl_io_num = P4SCAN_LCM_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_config, &bus));

    ESP_ERROR_CHECK(p4scan_lcm_power_init(bus));
    ESP_ERROR_CHECK(p4scan_lcm_backlight_init());
    ESP_ERROR_CHECK(p4scan_lcm_backlight_off());
    esp_err_t ret = p4scan_lcm_power_on();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCM power-on failed: %s; keeping the CPU alive for diagnosis",
                 esp_err_to_name(ret));
        // Return the PCA9538A outputs to a known safe state before waiting.
        esp_err_t off_ret = p4scan_lcm_power_off();
        if (off_ret != ESP_OK) {
            ESP_LOGE(TAG, "LCM power-off after startup failure also failed: %s",
                     esp_err_to_name(off_ret));
        }
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    esp_lcd_panel_handle_t panel = NULL;
    void *frame_buffer = NULL;
    ESP_ERROR_CHECK(p4scan_lcm_display_init(&panel, &frame_buffer));
    ESP_ERROR_CHECK(p4scan_lcm_set_backlight_enable(true));
    ESP_ERROR_CHECK(p4scan_lcm_backlight_set_brightness(50));

#if P4SCAN_DISPLAY_ONLY
    ESP_LOGI(TAG, "LCM display-only diagnostic is running: RGB565 snake animation, motion~20FPS, DPI~60Hz, backlight=50%%");
#else
    ESP_ERROR_CHECK(p4scan_touch_init(bus));
    ESP_LOGI(TAG, "LCM demo is running: checkerboard, touch IRQ GPIO=%d, backlight=50%%",
             P4SCAN_TOUCH_INT_GPIO);
#endif

    const uint32_t path_length = P4SCAN_SNAKE_COLS * P4SCAN_SNAKE_ROWS;
    uint32_t frame = 0;
    uint32_t head_step = 0;
    uint32_t food_step = 73 % path_length;
    int min_row;
    int max_row;
    draw_snake_state((uint16_t *)frame_buffer, head_step, food_step, &min_row, &max_row);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0,
                                               min_row * P4SCAN_SNAKE_CELL_SIZE,
                                               P4SCAN_LCM_H_RES,
                                               (max_row + 1) * P4SCAN_SNAKE_CELL_SIZE,
                                               frame_buffer));

    while (true) {
        uint32_t old_head_step = head_step;
        uint32_t old_food_step = food_step;
        head_step = (frame++ / P4SCAN_SNAKE_STEP_FRAMES) % path_length;
        food_step = (((frame / 240) * 137) + 73) % path_length;
        update_snake_frame((uint16_t *)frame_buffer, old_head_step, head_step,
                           old_food_step, food_step, &min_row, &max_row);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0,
                                                   min_row * P4SCAN_SNAKE_CELL_SIZE,
                                                   P4SCAN_LCM_H_RES,
                                                   (max_row + 1) * P4SCAN_SNAKE_CELL_SIZE,
                                                   frame_buffer));
        vTaskDelay(pdMS_TO_TICKS(P4SCAN_ANIMATION_FRAME_MS));
    }
}
