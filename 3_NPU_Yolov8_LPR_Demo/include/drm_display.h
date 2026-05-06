#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

#include "common.h"

struct DrmDisplay {
    int drm_fd = -1;
    uint32_t conn_id = 0;
    uint32_t crtc_id = 0;
    uint32_t fb_id = 0;
    uint32_t handle = 0;
    uint32_t pitch = 0;
    size_t size = 0;
    void* map = nullptr;
    int dmabuf_fd = -1;
    int width = 0;
    int height = 0;
    int mode_width = 0;
    int mode_height = 0;
    void* orig_crtc = nullptr;
};

int drm_display_init(DrmDisplay* disp, int width, int height);
int drm_display_present(DrmDisplay* disp, const image_buffer_t* src);
int drm_display_present_NV12(DrmDisplay* disp, const image_buffer_t* src);
void drm_display_deinit(DrmDisplay* disp);

#endif // DRM_DISPLAY_H
