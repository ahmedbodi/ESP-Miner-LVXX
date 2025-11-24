#ifndef DISPLAY_H_
#define DISPLAY_H_

#define DEFAULT_DISPLAY "ST7789 (240x240)" // Default to ST7789
#define LCD_SH1107_I2C_CMD                   0X00
#define LCD_SH1107_PARAM_SET_DISP_OFFSET     0xD3
#define LCD_SH1107_PARAM_DEFAULT_DISP_OFFSET 0x60

typedef enum
{
    NONE,
    SSD1306,
    SSD1309,
    SH1107,
    ST7789,
} Display;

typedef struct {
    const char * name;
    Display display;
    uint16_t h_res;
    uint16_t v_res;

} DisplayConfig;

static const DisplayConfig display_configs[] = {
    {
        .name = "NONE",
        .display = NONE,
    },
    {
        .name = "SSD1306 (128x32)",
        .display = SSD1306,
        .h_res = 128,
        .v_res = 32,
    },
    {
        .name = "SSD1309 (128x64)",
        .display = SSD1309,
        .h_res = 128,
        .v_res = 64,
    },
    {
        .name = "SH1107 (64x128)",
        .display = SH1107,
        .h_res = 64,
        .v_res = 128,
    },
    {
        .name = "SH1107 (128x128)",
        .display = SH1107,
        .h_res = 128,
        .v_res = 128,
    },
    {
        .name = DEFAULT_DISPLAY,
        .display = ST7789,
        .h_res = 170,
        .v_res = 320,
    }, // Default ST7789
};

esp_err_t display_init(void * pvParameters);
esp_err_t display_on(bool display_on);
const DisplayConfig * get_display_config(const char * name);

#endif /* DISPLAY_H_ */
