idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES esp_lcd
                                     esp_timer
                                     esp_driver_gpio
                                     esp_driver_spi
                                     esp_driver_i2c
                                     esp_driver_ledc
                                     esp_driver_sdspi
                                     esp_adc
                                     esp_hw_support
                                     heap
                                     spi_flash
                                     fatfs
                                     sdmmc
                                     lvgl
                                     hardware
                                     georgik__sdl
                                     georgik__sdl_bsp
                       INCLUDE_DIRS "")

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# Conditionally compile sdl_demo.c when SDL demo is enabled
if(CONFIG_XIAOMIAO_USE_SDL)
    target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdl_demo.c)
endif()
