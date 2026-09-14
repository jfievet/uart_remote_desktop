#ifndef SCREEN_SHARING_CAPTURE_DXGI_H
#define SCREEN_SHARING_CAPTURE_DXGI_H

#include "capture.h"

/* internal to the capture module -- not part of the public capture.h API */
int ss_capture_dxgi_capture(ss_frame_t *frame);
void ss_capture_dxgi_shutdown(void);

#endif
