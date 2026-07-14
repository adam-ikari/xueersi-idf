idf_component_register(SRCS
                       hw_board.c
                       hw_display.c
                       hw_input.c
                       hw_i2c.c
                       hw_gd32.c
                       hw_mpu.c
                       hw_adc.c
                       hw_buzzer.c
                       hw_extio.c
                       hw_sd.c
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_lcd esp_timer esp_driver_gpio esp_driver_spi
                                     esp_driver_i2c esp_driver_ledc esp_driver_sdspi
                                     esp_adc esp_hw_support heap spi_flash fatfs sdmmc)
