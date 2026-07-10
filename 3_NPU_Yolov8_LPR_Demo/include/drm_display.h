#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

struct DrmDisplay {
    int drm_fd = -1;
    uint32_t conn_id = 0;
    uint32_t crtc_id = 0;
    int crtc_index = -1;
    int mode_width = 0;
    int mode_height = 0;
    void* orig_crtc = nullptr;
    void* atomic_state = nullptr;
};

int drm_display_init(DrmDisplay* display, int width, int height);
int drm_display_get_ui_buffer(DrmDisplay* display, image_buffer_t* ui_buffer);
int drm_display_present(DrmDisplay* display, const image_buffer_t* image);
int drm_display_present_nv12(DrmDisplay* display, const image_buffer_t* video_buffer);
void drm_display_deinit(DrmDisplay* display);

#endif  // DRM_DISPLAY_H
