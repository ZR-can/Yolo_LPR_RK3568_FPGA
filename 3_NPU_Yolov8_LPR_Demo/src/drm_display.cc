#include "drm_display.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "xf86drm.h"
#include "drm.h"
#include "drm_mode.h"
#include "drm_fourcc.h"

#include "image_utils.h"
#include <linux/types.h>

#ifndef DMA_BUF_IOCTL_SYNC

struct dma_buf_sync {
    __u64 flags;
};

#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)

#define DMA_BUF_BASE           'b'
#define DMA_BUF_IOCTL_SYNC     _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

#endif
struct DrmCrtcState {
    drm_mode_crtc crtc;
    uint32_t conn_id = 0;
    int valid = 0;
};

static const uint32_t DRM_MODE_CONNECTED = 1;

static int drm_get_resources(int fd, drm_mode_card_res* res,
                             uint32_t** conn_ids, uint32_t** crtc_ids, uint32_t** enc_ids) {
    memset(res, 0, sizeof(*res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) != 0) {
        return -1;
    }

    *conn_ids = (uint32_t*)calloc(res->count_connectors, sizeof(uint32_t));
    *crtc_ids = (uint32_t*)calloc(res->count_crtcs, sizeof(uint32_t));
    *enc_ids = (uint32_t*)calloc(res->count_encoders, sizeof(uint32_t));
    if (!*conn_ids || !*crtc_ids || !*enc_ids) {
        return -1;
    }

    res->connector_id_ptr = (uint64_t)(uintptr_t)(*conn_ids);
    res->crtc_id_ptr = (uint64_t)(uintptr_t)(*crtc_ids);
    res->encoder_id_ptr = (uint64_t)(uintptr_t)(*enc_ids);
    res->count_fbs = 0;
    res->fb_id_ptr = 0;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) != 0) {
        return -1;
    }
    return 0;
}

static int drm_get_connector(int fd, uint32_t conn_id, drm_mode_get_connector* conn,
                             drm_mode_modeinfo** modes, uint32_t** encoders) {
    memset(conn, 0, sizeof(*conn));
    conn->connector_id = conn_id;
    
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
        return -1;
    }
    if (conn->count_modes == 0) {
        return 0;
    }

    *modes = (drm_mode_modeinfo*)calloc(conn->count_modes, sizeof(drm_mode_modeinfo));
    *encoders = (uint32_t*)calloc(conn->count_encoders, sizeof(uint32_t));
    if (!*modes || !*encoders) {
        return -1;
    }

    conn->modes_ptr = (uint64_t)(uintptr_t)(*modes);
    conn->encoders_ptr = (uint64_t)(uintptr_t)(*encoders);
    conn->count_props = 0;
    conn->props_ptr = 0;
    conn->prop_values_ptr = 0;

    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
        return -1;
    }
    return 0;
}

static int drm_get_encoder(int fd, uint32_t enc_id, drm_mode_get_encoder* enc) {
    memset(enc, 0, sizeof(*enc));
    enc->encoder_id = enc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, enc) != 0) {
        return -1;
    }
    return 0;
}

static int drm_get_crtc(int fd, uint32_t crtc_id, drm_mode_crtc* crtc) {
    memset(crtc, 0, sizeof(*crtc));
    crtc->crtc_id = crtc_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, crtc) != 0) {
        return -1;
    }
    return 0;
}

static int drm_set_crtc(int fd, drm_mode_crtc* crtc) {
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, crtc) != 0) {
        return -1;
    }
    return 0;
}

static int drm_add_fb2(int fd, drm_mode_fb_cmd2* cmd) {
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, cmd) != 0) {
        return -1;
    }
    return 0;
}

static int drm_pick_connector(int fd, uint32_t* conn_id, drm_mode_modeinfo* mode, uint32_t* crtc_id,
                              int want_width, int want_height) {
    drm_mode_card_res res;
    uint32_t* conn_ids = nullptr;
    uint32_t* crtc_ids = nullptr;
    uint32_t* enc_ids = nullptr;

    printf("DRM: Calling drm_get_resources...\n");
    if (drm_get_resources(fd, &res, &conn_ids, &crtc_ids, &enc_ids) != 0) {
        printf("DRM: drm_get_resources failed! Not a display device or permission denied.\n");
        return -1;
    }
    
    printf("DRM: Found %d connectors, %d CRTCs, %d encoders\n", 
           res.count_connectors, res.count_crtcs, res.count_encoders);

    int ret = -1;
    for (uint32_t i = 0; i < res.count_connectors; ++i) {
        drm_mode_get_connector conn;
        drm_mode_modeinfo* modes = nullptr;
        uint32_t* encoders = nullptr;

        if (drm_get_connector(fd, conn_ids[i], &conn, &modes, &encoders) != 0) {
            printf("DRM: Failed to get connector properties for ID %d\n", conn_ids[i]);
            free(modes); free(encoders);
            continue;
        }

        printf("DRM: Connector ID %d | Type: %d | Connection Status: %d | Modes count: %d\n",
               conn.connector_id, conn.connector_type, conn.connection, conn.count_modes);

        if (conn.connection != DRM_MODE_CONNECTED || conn.count_modes == 0) {
            printf("DRM: Skipping Connector ID %d (Not connected or no modes)\n", conn.connector_id);
            free(modes); free(encoders);
            continue;
        }

        if (conn.connector_type != DRM_MODE_CONNECTOR_HDMIA) {
            printf("DRM: Skipping Connector ID %d (Type %d is not HDMI)\n", conn.connector_id, conn.connector_type);
            free(modes); free(encoders);
            continue;
        }

        printf("DRM: SUCCESS! Found connected HDMI Connector ID %d\n", conn.connector_id);
        *conn_id = conn.connector_id;
        *mode = modes[0];
        
        // 寻找最接近目标分辨率的 mode
        for (uint32_t m = 0; m < conn.count_modes; ++m) {
            if (modes[m].hdisplay == want_width && modes[m].vdisplay == want_height) {
                *mode = modes[m];
                break;
            }
        }
        
 
        int crtc_found = 0;
        *crtc_id = 0;

        for (uint32_t e = 0; e < conn.count_encoders; ++e) {
            drm_mode_get_encoder enc;
            if (drm_get_encoder(fd, encoders[e], &enc) == 0) {
                for (uint32_t c = 0; c < res.count_crtcs; ++c) {
                    if (enc.possible_crtcs & (1 << c)) {
                        *crtc_id = crtc_ids[c];
                        crtc_found = 1;
                        printf("DRM: Found compatible CRTC ID %d via possible_crtcs\n", *crtc_id);
                        break;
                    }
                }
            }
            if (crtc_found) break;
        }

        if (!crtc_found && res.count_crtcs > 0) {
            *crtc_id = crtc_ids[0];
            printf("DRM: WARNING - No proper routing found, falling back to CRTC %d\n", *crtc_id);
        }

        free(modes);
        free(encoders);
        ret = 0;
        break;
    }

    free(conn_ids);
    free(crtc_ids);
    free(enc_ids);
    return ret;
}

static int drm_try_open_card(DrmDisplay* disp, const char* path, int width, int height, drm_mode_modeinfo* mode) {
    printf("DRM: -------- Trying to open %s --------\n", path);
    disp->drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (disp->drm_fd < 0) {
        printf("DRM: Failed to open %s\n", path);
        return -1;
    }
    
    printf("DRM: Successfully opened %s (fd=%d), probing connectors...\n", path, disp->drm_fd);
    if (drm_pick_connector(disp->drm_fd, &disp->conn_id, mode, &disp->crtc_id, width, height) != 0) {
        printf("DRM: Probe failed for %s\n", path);
        close(disp->drm_fd);
        disp->drm_fd = -1;
        return -1;
    }
    return 0;
}

int drm_display_init(DrmDisplay* disp, int width, int height) {
    if (!disp) {
        return -1;
    }

    drm_mode_modeinfo mode;
    memset(&mode, 0, sizeof(mode));
    const char* cards[] = {"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", "/dev/dri/card3"};
    int found = -1;
    for (size_t i = 0; i < sizeof(cards) / sizeof(cards[0]); ++i) {
        if (drm_try_open_card(disp, cards[i], width, height, &mode) == 0) {
            printf("DRM: using %s\n", cards[i]);
            found = 0;
            break;
        }
    }
    if (found != 0) {
        printf("DRM: no connected connector found\n");
        return -1;
    }

    disp->width = width;
    disp->height = height;
    disp->mode_width = mode.hdisplay;
    disp->mode_height = mode.vdisplay;

    drm_mode_crtc orig;
    if (drm_get_crtc(disp->drm_fd, disp->crtc_id, &orig) == 0) {
        DrmCrtcState* state = new DrmCrtcState();
        state->crtc = orig;
        state->conn_id = disp->conn_id;
        state->valid = 1;
        disp->orig_crtc = state;
    }

    struct drm_mode_create_dumb create = {};
    create.width = disp->mode_width;
    create.height = disp->mode_height;  // 【修改】不再乘以 3/2
    create.bpp = 32;                    // 【修改】32位真彩色，硬件兼容性 100%
    if (ioctl(disp->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        drm_display_deinit(disp);
        return -1;
    }

    disp->handle = create.handle;
    disp->pitch = create.pitch;
    disp->size = create.size;

    uint32_t handles[4] = {disp->handle, disp->handle, 0, 0};
    uint32_t pitches[4] = {disp->pitch, disp->pitch, 0, 0};
    uint32_t offsets[4] = {0, disp->pitch * (uint32_t)disp->mode_height, 0, 0};

    drm_mode_fb_cmd2 fb_cmd;
    memset(&fb_cmd, 0, sizeof(fb_cmd));
    fb_cmd.width = disp->mode_width;
    fb_cmd.height = disp->mode_height;
    // 使用 XBGR8888，完美契合 RGA 的 RGBA8888 内存布局
    fb_cmd.pixel_format = DRM_FORMAT_XBGR8888; 
    fb_cmd.handles[0] = disp->handle;
    fb_cmd.pitches[0] = disp->pitch;
    fb_cmd.offsets[0] = 0;
    if (drm_add_fb2(disp->drm_fd, &fb_cmd) != 0) {
        perror("DRM_IOCTL_MODE_ADDFB2");
        drm_display_deinit(disp);
        return -1;
    }
    disp->fb_id = fb_cmd.fb_id;

    struct drm_mode_map_dumb map = {};
    map.handle = disp->handle;
    if (ioctl(disp->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        drm_display_deinit(disp);
        return -1;
    }

    disp->map = mmap(0, disp->size, PROT_READ | PROT_WRITE, MAP_SHARED, disp->drm_fd, map.offset);
    if (disp->map == MAP_FAILED) {
        disp->map = nullptr;
        perror("mmap");
        drm_display_deinit(disp);
        return -1;
    }

    drm_mode_crtc set_crtc;
    memset(&set_crtc, 0, sizeof(set_crtc));
    set_crtc.crtc_id = disp->crtc_id;
    set_crtc.fb_id = disp->fb_id;
    set_crtc.x = 0;
    set_crtc.y = 0;
    set_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&disp->conn_id;
    set_crtc.count_connectors = 1;
    set_crtc.mode_valid = 1;
    set_crtc.mode = mode;
    if (drm_set_crtc(disp->drm_fd, &set_crtc) != 0) {
        perror("DRM_IOCTL_MODE_SETCRTC");
        drm_display_deinit(disp);
        return -1;
    }

    memset(disp->map, 0, disp->size);

    struct drm_prime_handle prime = {};
    prime.handle = disp->handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(disp->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        drm_display_deinit(disp);
        return -1;
    }
    disp->dmabuf_fd = prime.fd;
    return 0;
}

int drm_display_present(DrmDisplay* disp, const image_buffer_t* src) {
    if (!disp || !src || disp->dmabuf_fd < 0) {
        return -1;
    }

    image_buffer_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.width = disp->mode_width;
    dst.height = disp->mode_height;
    
    dst.width_stride = disp->pitch / 4; 
    dst.height_stride = disp->mode_height;
    dst.format = IMAGE_FORMAT_RGBA8888; 
    dst.fd = disp->dmabuf_fd;

    int dst_size = disp->pitch * disp->mode_height;
    dst.virt_addr = (uint8_t*)mmap(NULL, dst_size, PROT_READ | PROT_WRITE, MAP_SHARED, disp->dmabuf_fd, 0);
    
    if (dst.virt_addr == MAP_FAILED) {
        printf("drm_display_present: mmap failed, errno: %d\n", errno);
        return -1;
    }

    // 1. CPU 访问前：通知内核准备写入，如果必要会使缓存失效 (Invalidate)
    struct dma_buf_sync sync_start;
    sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    if (ioctl(disp->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync_start) < 0) {
        printf("DMA_BUF_SYNC_START failed\n");
    }

    image_buffer_t aligned_src = *src;
    aligned_src.width_stride = (aligned_src.width + 3) & ~3; 
    int ret = convert_image_with_letterbox(&aligned_src, &dst, NULL, 0);

    // 2. CPU 访问后：通知内核写入完成，强制将 CPU Cache 刷新到物理内存 DDR (Flush)
    struct dma_buf_sync sync_end;
    sync_end.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (ioctl(disp->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync_end) < 0) {
        printf("DMA_BUF_SYNC_END failed\n");
    }

    munmap(dst.virt_addr, dst_size);
    dst.virt_addr = NULL;

    return ret;
}

int drm_display_present_NV12(DrmDisplay* disp, const image_buffer_t* src) {
    if (!disp || !src || disp->dmabuf_fd < 0) {
        return -1;
    }

    image_buffer_t dst;
    memset(&dst, 0, sizeof(dst));
    dst.width = disp->mode_width;
    dst.height = disp->mode_height;
    
    // bpp=32意味着每个像素4字节。RGA 要求传入像素 stride
    dst.width_stride = disp->pitch / 4; 
    dst.height_stride = disp->mode_height;
    dst.format = IMAGE_FORMAT_RGBA8888; 
    dst.fd = disp->dmabuf_fd;
    return convert_image((image_buffer_t*)src, &dst, NULL,NULL, 0);
}

void drm_display_deinit(DrmDisplay* disp) {
    if (!disp || disp->drm_fd < 0) {
        return;
    }

    if (disp->orig_crtc) {
        DrmCrtcState* state = static_cast<DrmCrtcState*>(disp->orig_crtc);
        if (state->valid) {
            drm_mode_crtc restore = state->crtc;
            restore.set_connectors_ptr = (uint64_t)(uintptr_t)&state->conn_id;
            restore.count_connectors = 1;
            drm_set_crtc(disp->drm_fd, &restore);
        }
        delete state;
        disp->orig_crtc = nullptr;
    }

    if (disp->map) {
        munmap(disp->map, disp->size);
        disp->map = nullptr;
    }

    if (disp->dmabuf_fd >= 0) {
        close(disp->dmabuf_fd);
        disp->dmabuf_fd = -1;
    }

    if (disp->fb_id) {
        // 使用原生 ioctl，不依赖 libdrm
        ioctl(disp->drm_fd, DRM_IOCTL_MODE_RMFB, &disp->fb_id);
        disp->fb_id = 0;
    }

    close(disp->drm_fd);
    disp->drm_fd = -1;
}
