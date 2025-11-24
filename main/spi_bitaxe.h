#ifndef SPI_BITAXE_H_
#define SPI_BITAXE_H_

#include "driver/spi_master.h"

esp_err_t spi_bitaxe_init(void);
esp_err_t spi_bitaxe_add_device(spi_device_handle_t * dev_handle, const char * device_tag);

esp_err_t spi_bitaxe_register_read(spi_device_handle_t dev_handle, uint8_t reg_addr, uint8_t * read_buf, size_t len);
esp_err_t spi_bitaxe_register_write_addr(spi_device_handle_t dev_handle, uint8_t reg_addr);
esp_err_t spi_bitaxe_register_write_byte(spi_device_handle_t dev_handle, uint8_t reg_addr, uint8_t data);
esp_err_t spi_bitaxe_register_write_bytes(spi_device_handle_t dev_handle, uint8_t * data, uint8_t len);
esp_err_t spi_bitaxe_register_write_word(spi_device_handle_t dev_handle, uint8_t reg_addr, uint16_t data);

#endif /* SPI_BITAXE_H_ */
