#ifndef _EMU_BMP_H_
#define _EMU_BMP_H_

#include <stdint.h>

/* Save RGB565 framebuffer as 24-bit RGB BMP file.
 * fb: pointer to uint16_t RGB565 pixels (big-endian per TGL_PIXEL_BYTE_SWAP=1)
 * w, h: dimensions
 * path: output file path
 * Returns 0 on success, -1 on error.
 */
int bmp_save_rgb565(const uint16_t* fb, int w, int h, const char* path);

/* Save RGB565 framebuffer as 16-bit RGB565 BMP file (BI_BITFIELDS).
 * This preserves exact pixel values for comparison.
 */
int bmp_save_rgb565_raw(const uint16_t* fb, int w, int h, const char* path);

#endif
