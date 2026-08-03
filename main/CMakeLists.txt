idf_component_register(SRCS "main.c"
                                     "canvas_api.c"
                                     "input_api.c"
                                     "audio_api.c"
                                     "tinygl_test.c"
                                     "tinygl_physics.c"
                                     "render_queue.c"
                                     "wasm3_game.c"
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
                                     wasm3
                                     tinygl
                                     debug_console
                       INCLUDE_DIRS ""
                       PRIV_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(${COMPONENT_LIB} PRIVATE m)

# WASM3 game engine on core 0 (wasm game logic + physics)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TGL_WASM_GAME=1)

# Conditionally compile sdl_demo.c when SDL demo is enabled
if(CONFIG_XIAOMIAO_USE_SDL)
    target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdl_demo.c)
endif()

# Generate WASM bytecode from WAT (WebAssembly Text) at compile time.
# wat2wasm must be installed (brew install wabt or apt install wabt).
find_program(WAT2WASM wat2wasm)
if(WAT2WASM)
    set(WASM_SRC  ${CMAKE_CURRENT_SOURCE_DIR}/wasm_game.wat)
    set(WASM_FILE ${CMAKE_CURRENT_BINARY_DIR}/wasm_game.wasm)
    add_custom_command(
        OUTPUT ${WASM_FILE}
        COMMAND ${WAT2WASM} ${WASM_SRC} -o ${WASM_FILE}
        DEPENDS ${WASM_SRC}
        VERBATIM
    )
    # Generate C header embedding the WASM bytecode
    set(WASM_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wasm_game.wasm.h)
    add_custom_command(
        OUTPUT ${WASM_HEADER}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/bin2header.py
                ${WASM_FILE} ${WASM_HEADER} wasm_game_wasm
        DEPENDS ${WASM_FILE}
        VERBATIM
    )
    add_custom_target(gen_wasm_header DEPENDS ${WASM_HEADER})
    add_dependencies(${COMPONENT_LIB} gen_wasm_header)
else()
    # Fallback: embed the old pre-built test wasm
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
endif()

# Generate texture headers at compile time (tools/gen_texture.py).
# Each texture is a 256x256 RGB array embedded in flash (.rodata) and uploaded
# via glTexImage2D. Multiple textures enable multi-texture demos.
set(GEN_TEX ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_texture.py)
set(TEX_DIR ${CMAKE_CURRENT_BINARY_DIR})

set(TEXTURES ceramic checker brick grid sky sand horizon)
set(TEX_HEADERS "")
foreach(kind ${TEXTURES})
    set(hdr ${TEX_DIR}/texture_${kind}.h)
    list(APPEND TEX_HEADERS ${hdr})
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND python3 ${GEN_TEX} ${kind} ${hdr} texture_${kind} 128 128
        DEPENDS ${GEN_TEX}
        VERBATIM
    )
endforeach()
add_custom_target(gen_texture_headers DEPENDS ${TEX_HEADERS})
add_dependencies(${COMPONENT_LIB} gen_texture_headers)
