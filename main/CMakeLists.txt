idf_component_register(SRCS "main.c"
                                     "canvas_api.c"
                                     "input_api.c"
                                     "audio_api.c"
                                     "tinygl_test.c"
                                     "display_st7735.c"
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
                                     wasm-micro-runtime
                                     tinygl
                       INCLUDE_DIRS ""
                       PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# Conditionally compile sdl_demo.c when SDL demo is enabled
if(CONFIG_XIAOMIAO_USE_SDL)
    target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdl_demo.c)
endif()

# Generate C header embedding the WASM bytecode
set(WASM_FILE ${CMAKE_CURRENT_SOURCE_DIR}/wasm_test.wasm)
set(WASM_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wasm_test.wasm.h)
add_custom_command(
    OUTPUT ${WASM_HEADER}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/bin2header.py
            ${WASM_FILE} ${WASM_HEADER} wasm_test_wasm
    DEPENDS ${WASM_FILE}
    VERBATIM
)
add_custom_target(gen_wasm_header DEPENDS ${WASM_HEADER})
add_dependencies(${COMPONENT_LIB} gen_wasm_header)
