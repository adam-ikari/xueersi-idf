idf_component_register(
    SRCS debug_console.c
    INCLUDE_DIRS include
    PRIV_REQUIRES console
                  esp_vfs_console
                  esp_lcd
                  tinygl
                  hardware
)

# Match TinyGL's byte-swap setting so debug_console's encode/decode
# (rgb_to_pbuf, decode565) agree with what TinyGL writes to pbuf.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TGL_PIXEL_BYTE_SWAP=1
    TGL_TEXTURE_BYTE_SWAP=1
)
