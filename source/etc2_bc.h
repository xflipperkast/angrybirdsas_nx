#ifndef ABEPIC_ETC2_BC_H
#define ABEPIC_ETC2_BC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ETC2_BC_RGB8 = 1,
    ETC2_BC_RGBA8 = 2
};

size_t etc2_bc_image_size(int mode, int width, int height);
int etc2_bc_transcode(void *dst, size_t dst_size,
                      const void *src, size_t src_size,
                      int width, int height, int mode);

#ifdef __cplusplus
}
#endif

#endif
