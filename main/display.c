#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_st7789.h" // ST7789 driver
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh1107.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "i2c_bitaxe.h"
#include "lvgl.h"
#include "lvgl__lvgl/src/themes/lv_theme_private.h"
#include "nvs_config.h"
#include "rom/gpio.h"
#include "spi_bitaxe.h"
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define DISPLAY_I2C_ADDRESS    0x3C

#define LCD_CMD_BITS           8
#define LCD_PARAM_BITS         8
#define BL_GPIO                8 // active-low

static const char * TAG = "display";
static const char * LVGL_TAG = "lvgl";

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static bool display_state_on = false;

static lv_theme_t theme;
static lv_style_t scr_style;

extern const lv_font_t lv_font_portfolio_6x8;

esp_err_t display_on(bool display_on);

static void theme_apply(lv_theme_t *theme, lv_obj_t *obj) {
    if (lv_obj_get_parent(obj) == NULL) {
        lv_obj_add_style(obj, &scr_style, LV_PART_MAIN);
    }
}

static esp_err_t read_display_config(GlobalState * GLOBAL_STATE)
{
    char * display_config_name = nvs_config_get_string(NVS_CONFIG_DISPLAY);
    const DisplayConfig * display_config = get_display_config(display_config_name);

    if (display_config) {
        GLOBAL_STATE->DISPLAY_CONFIG = *display_config;

        ESP_LOGI(TAG, "%s", GLOBAL_STATE->DISPLAY_CONFIG.name);
        free(display_config_name);
        return ESP_OK;
    }

    free(display_config_name);
    return ESP_FAIL;
}

static void my_log_cb(lv_log_level_t level, const char * buf)
{
    switch (level) {
        case LV_LOG_LEVEL_TRACE:
            ESP_LOGV(LVGL_TAG, "%s", buf);
            break;
        case LV_LOG_LEVEL_INFO:
            ESP_LOGI(LVGL_TAG, "%s", buf);
            break;
        case LV_LOG_LEVEL_WARN:
            ESP_LOGW(LVGL_TAG, "%s", buf);
            break;
        case LV_LOG_LEVEL_ERROR:
            ESP_LOGE(LVGL_TAG, "%s", buf);
            break;
        case LV_LOG_LEVEL_USER:
            ESP_LOGI(LVGL_TAG, "%s", buf);
            break;
        case LV_LOG_LEVEL_NONE:
            break;
    }
}

static void bl_off(void)
{
    gpio_set_level(BL_GPIO, 1);
}
static void bl_on(void)
{
    gpio_set_level(BL_GPIO, 0);
}

static void bl_init(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << BL_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    bl_off();
}

typedef struct
{
    int w, h;
    int x_gap, y_gap;
    bool rgb_endian_rgb; // true=RGB, false=BGR
    const char * name;
} profile_t;


static void full_fill(esp_lcd_panel_handle_t panel, int w, int h, uint16_t color)
{
    // Fill screen by rows to avoid huge buffers
    uint16_t * line = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!line)
        return;
    for (int x = 0; x < w; x++)
        line[x] = color;
    for (int y = 0; y < h; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, w, y + 1, line);
    }
    free(line);
}


// Placeholder for SPI device handle
spi_device_handle_t spi_handle = NULL;

typedef struct
{
    int sclk, mosi, cs_native;
    spi_host_device_t host;
    const char * name;
} host_cfg_t;

static host_cfg_t hosts[] = {
    {.sclk = 12, .mosi = 11, .cs_native = 10, .host = SPI2_HOST, .name = "SPI2"},
};

static inline bool forbidden(int g)
{
    if (g == 0 || g == 45 || g == 46)
        return true; // strap/input-only
    if (g == 19 || g == 20)
        return true; // USB D-/D+
    if (g == 43 || g == 44)
        return true; // keep console alive by default
    if (g >= 22 && g <= 25)
        return true; // non-existent in QFN-56
    if (g >= 26 && g <= 37)
        return true;
    return false;
}

static int build_candidates(int * out, int max)
{
    int n = 0;
    for (int g = 1; g <= 48; ++g) {
        if (!forbidden(g))
            out[n++] = g;
        if (n == max)
            break;
    }
    return n;
}

static bool try_one(const host_cfg_t * hc, int cs, int dc)
{
    esp_err_t err;

    // setup bus
    spi_bus_config_t bus = {.mosi_io_num = hc->mosi,
                            .miso_io_num = -1,
                            .sclk_io_num = hc->sclk,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = 4096};
    spi_bus_free(hc->host);
    err = spi_bus_initialize(hc->host, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] spi_bus_initialize failed: %s", hc->name, esp_err_to_name(err));
        return false;
    }

    // IO
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {.cs_gpio_num = cs,
                                            .dc_gpio_num = dc,
                                            .spi_mode = 0,
                                            .pclk_hz = 10 * 1000 * 1000,
                                            .trans_queue_depth = 10,
                                            .lcd_cmd_bits = 8,
                                            .lcd_param_bits = 8,
                                            .flags = {.sio_mode = 0, .lsb_first = 0, .cs_high_active = 0, .dc_low_on_data = 0}};
    err = esp_lcd_new_panel_io_spi(hc->host, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s cs=%d dc=%d] new_panel_io_spi failed: %s", hc->name, cs, dc, esp_err_to_name(err));
        spi_bus_free(hc->host);
        return false;
    }

    // Panel
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .flags.reset_active_high = 0,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s cs=%d dc=%d] new_panel_st7789 failed: %s", hc->name, cs, dc, esp_err_to_name(err));
        esp_lcd_panel_io_del(io);
        spi_bus_free(hc->host);
        return false;
    }

    esp_lcd_panel_reset(panel);
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s cs=%d dc=%d] panel_init failed: %s", hc->name, cs, dc, esp_err_to_name(err));
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io);
        spi_bus_free(hc->host);
        return false;
    }

    bl_on();

    // Small draw from HEAP (DMA-capable)
    uint16_t * tile = heap_caps_malloc(32 * 32 * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!tile) {
        ESP_LOGE(TAG, "heap_caps_malloc failed");
        return false;
    }
    for (int i = 0; i < 32 * 32; i++)
        tile[i] = 0xF81F; // magenta
    err = esp_lcd_panel_draw_bitmap(panel, 0, 0, 32, 32, tile);
    free(tile);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s cs=%d dc=%d] draw failed: %s", hc->name, cs, dc, esp_err_to_name(err));
        esp_lcd_panel_del(panel);
        esp_lcd_panel_io_del(io);
        spi_bus_free(hc->host);
        bl_off();
        return false;
    }

    ESP_LOGE(TAG, ">>> MATCH: HOST=%s  MOSI=%d SCLK=%d  CS=%d  DC=%d  (BL=GPIO%d active-low)", hc->name, hc->mosi, hc->sclk, cs, dc,
             8);
    vTaskDelay(pdMS_TO_TICKS(4000));

    esp_lcd_panel_del(panel);
    esp_lcd_panel_io_del(io);
    spi_bus_free(hc->host);
    return true;
}

esp_err_t display_init(void * pvParameters)
{

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    ESP_RETURN_ON_ERROR(read_display_config(GLOBAL_STATE), TAG, "Failed to read display config");

    ESP_LOGI(TAG, "Starting full CS/DC brute-force (BL=GPIO%d active-low)", 8);

    bl_init();
    // Build the "all safe GPIOs" list
    int pins[64];
    int n = build_candidates(pins, 64);

    // Try both hosts; for each host, try CS=-1 first with all DC; then CS=each pin with all DC
    int num_hosts = sizeof(hosts) / sizeof(hosts[0]);
    for (int h = 0; h < num_hosts; ++h) {
        const host_cfg_t * hc = &hosts[h];
        ESP_LOGI(TAG, "---- HOST %s (MOSI=%d SCLK=%d native CS=%d) ----", hc->name, hc->mosi, hc->sclk, hc->cs_native);

        // 1) CS = -1 (tied low) sweep DC
        for (int di = 0; di < n; ++di) {
            int dc = pins[di];
            if (dc == 8)
                continue;
            if (dc == hc->sclk || dc == hc->mosi)
                continue;
            ESP_LOGW(TAG, "Trying HOST=%s  CS=-1  DC=%d ...", hc->name, dc);
            if (try_one(hc, -1, dc)) {
                ESP_LOGE(TAG, "LOCK on first success");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            bl_off();

        }

        // 2) CS sweep (all pins), with DC sweep (all pins)
        for (int ci = 0; ci < n; ++ci) {
            int cs = pins[ci];
            if (cs == 8)
                continue;
            // Avoid using bus pins as CS only if they collide with SCLK/MOSI? It's okay to use any GPIO as CS via matrix.
            // Skip if using the *other* host's native CS to reduce confusion is optional; here we allow all.
            for (int di = 0; di < n; ++di) {
                int dc = pins[di];
                if (dc == 8)
                    continue;
                if (dc == hc->sclk || dc == hc->mosi)
                    continue; // DC must be distinct from bus pins
                if (dc == cs)
                    continue; // keep CS and DC separate
                ESP_LOGW(TAG, "Trying HOST=%s  CS=%d  DC=%d ...", hc->name, cs, dc);
                if (try_one(hc, cs, dc)) {
                    ESP_LOGE(TAG, "LOCK on first success. Reboot to try more.");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
                bl_off();
            }
        }
    }

    ESP_LOGE(TAG, "No working CS/DC combination found. Consider checking RST or MOSI/SCLK host.");
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Initialize SPI LCD");

    // ---- Backlight (GPIO 8, ACTIVE-LOW): keep OFF during init ----
    ESP_LOGI(TAG, "Turn OFF LCD backlight (active-low on GPIO8)");
    gpio_reset_pin(8);
    gpio_set_direction(8, GPIO_MODE_OUTPUT);
    gpio_set_level(8, 1); // OFF (active-low)

    ESP_LOGI(TAG, "=============Starting Display Initialization Sequence============");

// CS could be -1 if tied to GND, but we leave it as GPIO5 for now
// OR 10 for SPI2_HOST
// OR 39 for SPI3_HOST

    esp_lcd_panel_io_spi_config_t spi_io_cfg = {
        .cs_gpio_num = 10,
        .dc_gpio_num = 6,
        .spi_mode = 0,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        //.flags.dc_low_on_data = 0,
        //.flags.lsb_first = 0,
        //.flags.sio_mode = 0,
        //.flags.cs_high_active = 0,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &spi_io_cfg, &io_handle), TAG, "Failed to init SPI LCD bus");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        //.flags.reset_active_high = 0,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };

    // Initialize panel
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle), TAG, "No display found");

    ESP_LOGI(TAG, "ST7789 panel initialized");
    
    ESP_LOGI(TAG, "Panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "Panel reset failed");
    ESP_LOGI(TAG, "Panel reset complete");

    ESP_LOGI(TAG, "Panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "Panel init failed");
    ESP_LOGI(TAG, "Panel init complete");

    // Optional orientation/invert after basic init
    bool invert_screen = nvs_config_get_bool(NVS_CONFIG_INVERT_SCREEN);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel_handle, invert_screen), TAG, "Panel invert failed");

    ESP_LOGI(TAG, "Enable panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "Disp on failed");

    // ---- Turn BL ON only after panel is ready (ACTIVE-LOW) ----
    gpio_set_level(8, 0);

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192; // give LVGL its own stack
    lvgl_cfg.task_priority = 5;
    lvgl_cfg.task_stack_caps = MALLOC_CAP_INTERNAL; // internal RAM stack
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL init failed");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = (GLOBAL_STATE->DISPLAY_CONFIG.h_res * GLOBAL_STATE->DISPLAY_CONFIG.v_res) * 2,
        .double_buffer = false,
        .hres = GLOBAL_STATE->DISPLAY_CONFIG.h_res,
        .vres = GLOBAL_STATE->DISPLAY_CONFIG.v_res,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .swap_bytes = false,
            .sw_rotate = false,
        },
    };

    lv_disp_t * disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) { // Check if disp is NULL
        ESP_LOGE(TAG, "lvgl_port_add_disp failed!");
        return ESP_FAIL;
    }

    if (lvgl_port_lock(0)) {
        uint16_t rotation = nvs_config_get_u16(NVS_CONFIG_ROTATION);
        ESP_LOGI(TAG, "Rotation: %d", rotation);
        switch(rotation) {
            case 90:
                lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
                break;
            case 180:
                lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
                break;
            case 270:
                lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
                break;
        }

        lv_style_init(&scr_style);
        lv_style_set_text_font(&scr_style, &lv_font_portfolio_6x8);
        lv_style_set_bg_opa(&scr_style, LV_OPA_COVER);

        lv_theme_set_apply_cb(&theme, theme_apply);

        lv_display_set_theme(disp, &theme);
        lvgl_port_unlock();
    }

    // Only turn on the screen when it has been cleared
    ESP_RETURN_ON_ERROR(display_on(true), TAG, "Display on failed");
    GLOBAL_STATE->SYSTEM_MODULE.is_screen_active = true;

    ESP_LOGI(TAG, "Display init success!");
    return ESP_OK;
}

esp_err_t display_on(bool display_on)
{
    if (NULL != panel_handle) {
        if (display_on && !display_state_on) {
            ESP_LOGI(TAG, "Turning display on");
            ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "Panel display on failed");
            display_state_on = true;
        }
        else if (!display_on && display_state_on)
        {
            ESP_LOGI(TAG, "Turning display off");
            ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, false), TAG, "Panel display off failed");
            display_state_on = false;
        }
    } else {
        ESP_LOGW(TAG, "No panel handle, cannot change display state");
    }

    return ESP_OK;
}

const DisplayConfig * get_display_config(const char * name)
{
    for (int i = 0 ; i < ARRAY_SIZE(display_configs); i++) {
        if (strcmp(display_configs[i].name, name) == 0) {
            return &display_configs[i];
        }
    }
    return NULL;
}
