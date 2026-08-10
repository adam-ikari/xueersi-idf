idf_component_register(SRCS "main.c"
                                     "canvas_api.c"
                                     "input_api.c"
                                     "audio_api.c"
                                     "tinygl_test.c"
                                     "tinygl_physics.c"
                                     "render_queue.c"
                                     "glcmd_stream.c"
                                     "wasm_game.c"
                                     "res_manager.c"
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

# WAMR game engine on core 0 (wasm game logic)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TGL_WASM_GAME=1)

# Display framebuffer count. 3 buffers (render/post/DMA) consume 3×40KB = 120KB
# of internal SRAM; with TinyGL's zbuf also forced internal (40KB) the 3rd buffer
# tips internal SRAM over and ZB_open fails at boot (ZB_open → gl_malloc zbuf
# returns NULL). 2 buffers (render/DMA) free 40KB so the zbuf fits. gl_post_process
# is a no-op so the post buffer is unused anyway; DMA 5.5ms vs 33ms frame means the
# 2-buffer wait_dma() almost never blocks. Override the #define in display_st7735.c.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DISPLAY_NUM_BUFFERS=2)

# Conditionally compile sdl_demo.c when SDL demo is enabled
if(CONFIG_XIAOMIAO_USE_SDL)
    target_sources(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sdl_demo.c)
endif()

# Generate WASM bytecode from wasm_game.cpp (C++ → wasm32) at compile time.
# clang++ with the wasm32 target must be installed.
find_program(CLANGXX clang++)
if(CLANGXX)
    set(WASM_SRC  ${CMAKE_CURRENT_SOURCE_DIR}/wasm_game.cpp)
    set(WASM_OBJ  ${CMAKE_CURRENT_BINARY_DIR}/wasm_game.o)
    set(WASM_FILE ${CMAKE_CURRENT_BINARY_DIR}/wasm_game.wasm)
    add_custom_command(
        OUTPUT ${WASM_OBJ}
        COMMAND ${CLANGXX} --target=wasm32 -O3 -nostdlib -c ${WASM_SRC} -o ${WASM_OBJ}
        DEPENDS ${WASM_SRC}
        VERBATIM
    )
    find_program(WASMLD wasm-ld)
    add_custom_command(
        OUTPUT ${WASM_FILE}
        COMMAND ${WASMLD} --no-entry --export-all --allow-undefined
                --initial-memory=131072 --max-memory=131072 ${WASM_OBJ} -o ${WASM_FILE}
        DEPENDS ${WASM_OBJ}
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

set(TEXTURES sky horizon sand metal reflect brick)
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
