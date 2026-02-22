#ifndef _ENCODE_VIDEO_H
#define _ENCODE_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int init_encoder(const char *output_file, int w, int h, int fps);
int encode_frame(uint8_t *img_buf, int img_buf_size);
void close_encoder();

#ifdef __cplusplus
}
#endif

#endif //_ENCODE_VIDEO_H