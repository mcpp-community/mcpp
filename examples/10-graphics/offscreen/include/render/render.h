#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Render one frame into `rgba`, which holds `w * h` pixels of four bytes each
// in R, G, B, A order. Returns 0 on success.
//
// The interface is a raw buffer for the reason every device seam in this
// repository uses one: the memory the device writes is not the program's, and
// the module above turns the result back into C++.
int render_offscreen(unsigned w, unsigned h, unsigned char* rgba);

// The clear colour the implementation uses, so the assertion and the renderer
// cannot disagree about it. Four bytes, R G B A.
void render_clear_color(unsigned char* rgba4);

// WHICH DEVICE THE LAST SUCCESSFUL `render_offscreen` RAN ON, or "" if none
// has. Both implementations produce the same image, so the image alone does not
// distinguish a device run from the software one -- which is the confusion an
// example about GPU rendering must not teach.
const char* render_device_name(void);

#ifdef __cplusplus
}
#endif
