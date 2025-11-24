#include "spi_bitaxe.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

#define SPI_HOST SPI2_HOST
#define SPI_CLOCK_HZ 8000000
#define SPI_DEFAULT_TIMEOUT -1

static const char * TAG = "spi_bitaxe";

typedef struct
{
    spi_device_handle_t handle;
    char device_tag[32];
} spi_dev_entry_t;

#define MAX_DEVICES 12
static spi_dev_entry_t dev_map[MAX_DEVICES];
static int dev_count = 0;
#define BUFFER_SZ (320 * 30)

/* -------------------------------------------------- */
/* SPI INITIALIZATION                                 */
/* -------------------------------------------------- */
esp_err_t spi_bitaxe_init(void)
{
    spi_bus_config_t cfg = {
        .mosi_io_num = CONFIG_GPIO_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_GPIO_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = BUFFER_SZ * sizeof(uint16_t) + 8
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST, &cfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");
    return ESP_OK;
}

/* -------------------------------------------------- */
/* ADD DEVICE                                         */
/* -------------------------------------------------- */
esp_err_t spi_bitaxe_add_device(spi_device_handle_t * dev_handle, const char * device_tag)
{
    if (dev_count >= MAX_DEVICES)
        return ESP_FAIL;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_GPIO_SPI_CS,
        .queue_size = 4,
    };

    esp_err_t err = spi_bus_add_device(SPI_HOST, &devcfg, dev_handle);
    if (err != ESP_OK)
        return err;

    dev_map[dev_count].handle = *dev_handle;
    strncpy(dev_map[dev_count].device_tag, device_tag, sizeof(dev_map[dev_count].device_tag) - 1);
    dev_count++;

    return ESP_OK;
}

/* -------------------------------------------------- */
/* REGISTER READ                                      */
/* -------------------------------------------------- */
esp_err_t spi_bitaxe_register_read(spi_device_handle_t dev_handle, uint8_t reg_addr, uint8_t * read_buf, size_t len)
{
    uint8_t tx[1] = {reg_addr | 0x80};
    uint8_t rx[len + 1];

    spi_transaction_t t = {.length = (len + 1) * 8, .tx_buffer = tx, .rx_buffer = rx};

    esp_err_t err = spi_device_transmit(dev_handle, &t);
    if (err != ESP_OK)
        return err;

    memcpy(read_buf, rx + 1, len);
    return ESP_OK;
}

/* -------------------------------------------------- */
/* REGISTER WRITE FUNCTIONS                           */
/* -------------------------------------------------- */
esp_err_t spi_bitaxe_register_write_addr(spi_device_handle_t dev_handle, uint8_t reg_addr)
{
    uint8_t tx = reg_addr & 0x7F;

    spi_transaction_t t = {.length = 8, .tx_buffer = &tx};

    return spi_device_transmit(dev_handle, &t);
}

esp_err_t spi_bitaxe_register_write_byte(spi_device_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t tx[2] = {reg_addr & 0x7F, data};

    spi_transaction_t t = {.length = 16, .tx_buffer = tx};

    return spi_device_transmit(dev_handle, &t);
}

esp_err_t spi_bitaxe_register_write_bytes(spi_device_handle_t dev_handle, uint8_t * data, uint8_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};

    return spi_device_transmit(dev_handle, &t);
}

esp_err_t spi_bitaxe_register_write_word(spi_device_handle_t dev_handle, uint8_t reg_addr, uint16_t data)
{
    uint8_t tx[3] = {reg_addr & 0x7F, (uint8_t) (data & 0xFF), (uint8_t) ((data >> 8) & 0xFF)};

    spi_transaction_t t = {.length = 24, .tx_buffer = tx};

    return spi_device_transmit(dev_handle, &t);
}
