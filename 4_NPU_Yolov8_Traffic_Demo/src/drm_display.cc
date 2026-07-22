#include "drm_display.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "drm.h"
#include "drm_fourcc.h"
#include "drm_mode.h"
#include "image_utils.h"

namespace {

constexpr uint32_t kDrmModeConnected = 1;
constexpr uint32_t kUiFormat = DRM_FORMAT_ABGR8888;

struct DrmCrtcState {
    drm_mode_crtc crtc{};
    uint32_t conn_id = 0;
    bool valid = false;
};

struct DumbBuffer {
    uint32_t handle = 0;
    uint32_t fb_id = 0;
    uint32_t pitch = 0;
    size_t size = 0;
    int dmabuf_fd = -1;
};

struct VideoFramebuffer {
    int dmabuf_fd = -1;
    int width = 0;
    int height = 0;
    uint32_t stride = 0;
    uint32_t height_stride = 0;
    uint32_t fourcc = 0;
    uint32_t fb_id = 0;
    uint32_t handle = 0;
};

struct PlaneProperties {
    uint32_t id = 0;
    std::unordered_map<std::string, uint32_t> property_ids;
};

struct AtomicRequest {
    std::vector<uint32_t> object_ids;
    std::vector<uint32_t> property_counts;
    std::vector<uint32_t> property_ids;
    std::vector<uint64_t> property_values;

    void Add(uint32_t object_id, uint32_t property_id, uint64_t value) {
        if (property_id == 0) {
            return;
        }
        size_t object_index = 0;
        while (object_index < object_ids.size() && object_ids[object_index] != object_id) {
            ++object_index;
        }
        if (object_index == object_ids.size()) {
            object_ids.push_back(object_id);
            property_counts.push_back(0);
        }
        ++property_counts[object_index];
        property_ids.push_back(property_id);
        property_values.push_back(value);
    }
};

struct DrmAtomicState {
    drm_mode_modeinfo mode{};
    uint32_t mode_blob_id = 0;
    PlaneProperties video_plane;
    PlaneProperties ui_plane;
    PlaneProperties crtc;
    PlaneProperties connector;
    DumbBuffer ui_buffers[2];
    std::vector<VideoFramebuffer> video_framebuffers;
    int current_ui_index = -1;
    int write_ui_index = 0;
    bool initialized = false;
    bool ui_commit_error_reported = false;
};

uint32_t GetPropertyId(const PlaneProperties& object, const char* name) {
    const auto found = object.property_ids.find(name);
    return found == object.property_ids.end() ? 0 : found->second;
}

int GetResources(int fd, drm_mode_card_res* resources, std::vector<uint32_t>* connectors,
                 std::vector<uint32_t>* crtcs, std::vector<uint32_t>* encoders) {
    memset(resources, 0, sizeof(*resources));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, resources) != 0) {
        return -1;
    }
    connectors->resize(resources->count_connectors);
    crtcs->resize(resources->count_crtcs);
    encoders->resize(resources->count_encoders);
    resources->connector_id_ptr = reinterpret_cast<uint64_t>(connectors->data());
    resources->crtc_id_ptr = reinterpret_cast<uint64_t>(crtcs->data());
    resources->encoder_id_ptr = reinterpret_cast<uint64_t>(encoders->data());
    resources->count_fbs = 0;
    resources->fb_id_ptr = 0;
    return ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, resources) == 0 ? 0 : -1;
}

int GetConnector(int fd, uint32_t connector_id, drm_mode_get_connector* connector,
                 std::vector<drm_mode_modeinfo>* modes, std::vector<uint32_t>* encoders) {
    memset(connector, 0, sizeof(*connector));
    connector->connector_id = connector_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector) != 0 || connector->count_modes == 0) {
        return -1;
    }
    modes->resize(connector->count_modes);
    encoders->resize(connector->count_encoders);
    connector->modes_ptr = reinterpret_cast<uint64_t>(modes->data());
    connector->encoders_ptr = reinterpret_cast<uint64_t>(encoders->data());
    connector->count_props = 0;
    connector->props_ptr = 0;
    connector->prop_values_ptr = 0;
    return ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, connector) == 0 ? 0 : -1;
}

int GetEncoder(int fd, uint32_t encoder_id, drm_mode_get_encoder* encoder) {
    memset(encoder, 0, sizeof(*encoder));
    encoder->encoder_id = encoder_id;
    return ioctl(fd, DRM_IOCTL_MODE_GETENCODER, encoder) == 0 ? 0 : -1;
}

int GetCrtc(int fd, uint32_t crtc_id, drm_mode_crtc* crtc) {
    memset(crtc, 0, sizeof(*crtc));
    crtc->crtc_id = crtc_id;
    return ioctl(fd, DRM_IOCTL_MODE_GETCRTC, crtc) == 0 ? 0 : -1;
}

int PickConnector(int fd, int requested_width, int requested_height, uint32_t* connector_id,
                  uint32_t* crtc_id, int* crtc_index, drm_mode_modeinfo* mode) {
    drm_mode_card_res resources{};
    std::vector<uint32_t> connectors;
    std::vector<uint32_t> crtcs;
    std::vector<uint32_t> encoders;
    if (GetResources(fd, &resources, &connectors, &crtcs, &encoders) != 0) {
        return -1;
    }

    for (uint32_t candidate : connectors) {
        drm_mode_get_connector connector{};
        std::vector<drm_mode_modeinfo> modes;
        std::vector<uint32_t> connector_encoders;
        if (GetConnector(fd, candidate, &connector, &modes, &connector_encoders) != 0 ||
            connector.connection != kDrmModeConnected) {
            continue;
        }

        int selected_crtc_index = -1;
        for (uint32_t encoder_id : connector_encoders) {
            drm_mode_get_encoder encoder{};
            if (GetEncoder(fd, encoder_id, &encoder) != 0) {
                continue;
            }
            for (size_t index = 0; index < crtcs.size(); ++index) {
                if (encoder.possible_crtcs & (1U << index)) {
                    selected_crtc_index = static_cast<int>(index);
                    break;
                }
            }
            if (selected_crtc_index >= 0) {
                break;
            }
        }
        if (selected_crtc_index < 0) {
            continue;
        }

        const drm_mode_modeinfo* selected_mode = nullptr;
        for (const drm_mode_modeinfo& candidate_mode : modes) {
            if ((candidate_mode.type & DRM_MODE_TYPE_PREFERRED) != 0) {
                selected_mode = &candidate_mode;
                break;
            }
        }
        if (selected_mode == nullptr) {
            for (const drm_mode_modeinfo& candidate_mode : modes) {
                if (candidate_mode.hdisplay == requested_width &&
                    candidate_mode.vdisplay == requested_height) {
                    selected_mode = &candidate_mode;
                    break;
                }
            }
        }
        *mode = selected_mode != nullptr ? *selected_mode : modes.front();
        *connector_id = connector.connector_id;
        *crtc_index = selected_crtc_index;
        *crtc_id = crtcs[selected_crtc_index];
        return 0;
    }
    return -1;
}

int EnableAtomicCapabilities(int fd) {
    drm_set_client_cap cap{};
    cap.capability = DRM_CLIENT_CAP_ATOMIC;
    cap.value = 1;
    if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) != 0) {
        return -1;
    }
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value = 1;
    return ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap) == 0 ? 0 : -1;
}

int ReadProperties(int fd, uint32_t object_id, uint32_t object_type, PlaneProperties* properties) {
    drm_mode_obj_get_properties object{};
    object.obj_id = object_id;
    object.obj_type = object_type;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object) != 0) {
        return -1;
    }
    std::vector<uint32_t> property_ids(object.count_props);
    std::vector<uint64_t> property_values(object.count_props);
    object.props_ptr = reinterpret_cast<uint64_t>(property_ids.data());
    object.prop_values_ptr = reinterpret_cast<uint64_t>(property_values.data());
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &object) != 0) {
        return -1;
    }

    properties->id = object_id;
    properties->property_ids.clear();
    for (uint32_t property_id : property_ids) {
        drm_mode_get_property property{};
        property.prop_id = property_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) == 0) {
            properties->property_ids[property.name] = property_id;
        }
    }
    return 0;
}

bool PlaneSupportsFormat(int fd, uint32_t plane_id, int crtc_index, uint32_t format) {
    drm_mode_get_plane plane{};
    plane.plane_id = plane_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 || (plane.possible_crtcs & (1U << crtc_index)) == 0) {
        return false;
    }
    std::vector<uint32_t> formats(plane.count_format_types);
    plane.format_type_ptr = reinterpret_cast<uint64_t>(formats.data());
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &plane) != 0) {
        return false;
    }
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}

int FindUiPlane(int fd, int crtc_index, DrmAtomicState* state) {
    drm_mode_get_plane_res resources{};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources) != 0) {
        return -1;
    }
    std::vector<uint32_t> plane_ids(resources.count_planes);
    resources.plane_id_ptr = reinterpret_cast<uint64_t>(plane_ids.data());
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources) != 0) {
        return -1;
    }

    uint32_t ui_plane_id = 0;
    for (uint32_t plane_id : plane_ids) {
        if (ui_plane_id == 0 && PlaneSupportsFormat(fd, plane_id, crtc_index, kUiFormat)) {
            ui_plane_id = plane_id;
        }
    }
    if (ui_plane_id == 0) {
        return -1;
    }
    return ReadProperties(fd, ui_plane_id, DRM_MODE_OBJECT_PLANE, &state->ui_plane);
}

int FindVideoPlane(int fd, int crtc_index, uint32_t video_format, DrmAtomicState* state) {
    drm_mode_get_plane_res resources{};
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources) != 0) {
        return -1;
    }
    std::vector<uint32_t> plane_ids(resources.count_planes);
    resources.plane_id_ptr = reinterpret_cast<uint64_t>(plane_ids.data());
    if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &resources) != 0) {
        return -1;
    }
    for (uint32_t plane_id : plane_ids) {
        if (plane_id != state->ui_plane.id && PlaneSupportsFormat(fd, plane_id, crtc_index, video_format)) {
            return ReadProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE, &state->video_plane);
        }
    }
    return -1;
}

int CreateUiBuffer(int fd, int width, int height, DumbBuffer* buffer) {
    drm_mode_create_dumb create{};
    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        return -1;
    }
    buffer->handle = create.handle;
    buffer->pitch = create.pitch;
    buffer->size = create.size;

    drm_mode_fb_cmd2 fb{};
    fb.width = width;
    fb.height = height;
    fb.pixel_format = kUiFormat;
    fb.handles[0] = buffer->handle;
    fb.pitches[0] = buffer->pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0) {
        return -1;
    }
    buffer->fb_id = fb.fb_id;

    drm_prime_handle prime{};
    prime.handle = buffer->handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        return -1;
    }
    buffer->dmabuf_fd = prime.fd;
    return 0;
}

void DestroyUiBuffer(int fd, DumbBuffer* buffer) {
    if (buffer->dmabuf_fd >= 0) {
        close(buffer->dmabuf_fd);
        buffer->dmabuf_fd = -1;
    }
    if (buffer->fb_id != 0) {
        ioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id);
        buffer->fb_id = 0;
    }
    if (buffer->handle != 0) {
        drm_mode_destroy_dumb destroy{};
        destroy.handle = buffer->handle;
        ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        buffer->handle = 0;
    }
}

uint32_t GetVideoFourcc(const image_buffer_t* video) {
    if (video->format == IMAGE_FORMAT_YUV420SP_NV12) {
        return DRM_FORMAT_NV12;
    }
    if (video->format == IMAGE_FORMAT_YUV420SP_NV21) {
        return DRM_FORMAT_NV21;
    }
    return 0;
}

void DestroyVideoFramebuffer(int fd, VideoFramebuffer* buffer) {
    if (buffer->fb_id != 0) {
        ioctl(fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id);
        buffer->fb_id = 0;
    }
    if (buffer->handle != 0) {
        drm_gem_close close_handle{};
        close_handle.handle = buffer->handle;
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
        buffer->handle = 0;
    }
}

void DestroyVideoFramebuffers(int fd, DrmAtomicState* state) {
    for (VideoFramebuffer& buffer : state->video_framebuffers) {
        DestroyVideoFramebuffer(fd, &buffer);
    }
    state->video_framebuffers.clear();
}

bool SameVideoLayout(const VideoFramebuffer& buffer, const image_buffer_t* video, uint32_t fourcc) {
    const uint32_t stride = video->width_stride > 0 ? static_cast<uint32_t>(video->width_stride)
                                                     : static_cast<uint32_t>(video->width);
    const uint32_t height_stride = video->height_stride > 0 ? static_cast<uint32_t>(video->height_stride)
                                                             : static_cast<uint32_t>(video->height);
    return buffer.width == video->width && buffer.height == video->height &&
           buffer.stride == stride && buffer.height_stride == height_stride && buffer.fourcc == fourcc;
}

int ImportVideoFramebuffer(int fd, const image_buffer_t* video, uint32_t fourcc, VideoFramebuffer* buffer) {
    if (video->fd < 0 || video->width <= 0 || video->height <= 0) {
        return -1;
    }

    drm_prime_handle prime{};
    prime.fd = video->fd;
    if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0) {
        return -1;
    }

    const uint32_t stride = video->width_stride > 0 ? static_cast<uint32_t>(video->width_stride)
                                                     : static_cast<uint32_t>(video->width);
    const uint32_t height_stride = video->height_stride > 0 ? static_cast<uint32_t>(video->height_stride)
                                                             : static_cast<uint32_t>(video->height);
    drm_mode_fb_cmd2 fb{};
    fb.width = video->width;
    fb.height = video->height;
    fb.pixel_format = fourcc;
    fb.handles[0] = prime.handle;
    fb.handles[1] = prime.handle;
    fb.pitches[0] = stride;
    fb.pitches[1] = stride;
    fb.offsets[1] = stride * height_stride;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) != 0) {
        drm_gem_close close_handle{};
        close_handle.handle = prime.handle;
        ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_handle);
        return -1;
    }

    buffer->dmabuf_fd = video->fd;
    buffer->width = video->width;
    buffer->height = video->height;
    buffer->stride = stride;
    buffer->height_stride = height_stride;
    buffer->fourcc = fourcc;
    buffer->fb_id = fb.fb_id;
    buffer->handle = prime.handle;
    return 0;
}

int GetVideoFramebuffer(int fd, DrmAtomicState* state, const image_buffer_t* video, uint32_t* fb_id) {
    const uint32_t fourcc = GetVideoFourcc(video);
    if (fourcc == 0) {
        return -1;
    }

    for (const VideoFramebuffer& buffer : state->video_framebuffers) {
        if (buffer.dmabuf_fd == video->fd && SameVideoLayout(buffer, video, fourcc)) {
            *fb_id = buffer.fb_id;
            return 0;
        }
    }

    VideoFramebuffer buffer;
    if (ImportVideoFramebuffer(fd, video, fourcc, &buffer) != 0) {
        return -1;
    }
    state->video_framebuffers.push_back(buffer);
    *fb_id = buffer.fb_id;
    return 0;
}

void PurgeVideoFramebuffersWithOtherLayouts(int fd, DrmAtomicState* state,
                                             const image_buffer_t* video) {
    const uint32_t fourcc = GetVideoFourcc(video);
    for (std::vector<VideoFramebuffer>::iterator it = state->video_framebuffers.begin();
         it != state->video_framebuffers.end();) {
        if (!SameVideoLayout(*it, video, fourcc)) {
            DestroyVideoFramebuffer(fd, &(*it));
            it = state->video_framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

void AddPlaneRequest(AtomicRequest* request, const PlaneProperties& plane, uint32_t crtc_id,
                     uint32_t fb_id, int src_width, int src_height,
                     int dst_width, int dst_height, int zpos) {
    request->Add(plane.id, GetPropertyId(plane, "FB_ID"), fb_id);
    request->Add(plane.id, GetPropertyId(plane, "CRTC_ID"), crtc_id);
    request->Add(plane.id, GetPropertyId(plane, "SRC_X"), 0);
    request->Add(plane.id, GetPropertyId(plane, "SRC_Y"), 0);
    request->Add(plane.id, GetPropertyId(plane, "SRC_W"), static_cast<uint64_t>(src_width) << 16);
    request->Add(plane.id, GetPropertyId(plane, "SRC_H"), static_cast<uint64_t>(src_height) << 16);
    request->Add(plane.id, GetPropertyId(plane, "CRTC_X"), 0);
    request->Add(plane.id, GetPropertyId(plane, "CRTC_Y"), 0);
    request->Add(plane.id, GetPropertyId(plane, "CRTC_W"), dst_width);
    request->Add(plane.id, GetPropertyId(plane, "CRTC_H"), dst_height);
    request->Add(plane.id, GetPropertyId(plane, "zpos"), zpos);
    request->Add(plane.id, GetPropertyId(plane, "alpha"), 0xffff);
}

int CommitAtomic(int fd, const AtomicRequest& request, uint32_t flags) {
    drm_mode_atomic atomic{};
    atomic.flags = flags;
    atomic.count_objs = request.object_ids.size();
    atomic.objs_ptr = reinterpret_cast<uint64_t>(request.object_ids.data());
    atomic.count_props_ptr = reinterpret_cast<uint64_t>(request.property_counts.data());
    atomic.props_ptr = reinterpret_cast<uint64_t>(request.property_ids.data());
    atomic.prop_values_ptr = reinterpret_cast<uint64_t>(request.property_values.data());
    return ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == 0 ? 0 : -1;
}

void AddModesetRequest(AtomicRequest* request, const DrmAtomicState& state, const DrmDisplay* display) {
    request->Add(state.connector.id, GetPropertyId(state.connector, "CRTC_ID"), display->crtc_id);
    request->Add(state.crtc.id, GetPropertyId(state.crtc, "MODE_ID"), state.mode_blob_id);
    request->Add(state.crtc.id, GetPropertyId(state.crtc, "ACTIVE"), 1);
}

int OpenDisplayCard(DrmDisplay* display, int width, int height, drm_mode_modeinfo* mode) {
    const char* cards[] = {"/dev/dri/card0", "/dev/dri/card1", "/dev/dri/card2", "/dev/dri/card3"};
    for (const char* card : cards) {
        const int fd = open(card, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) != 0) {
            fprintf(stderr,
                    "DRM: cannot acquire master on %s: %s; stop the desktop display service first\n",
                    card, strerror(errno));
            close(fd);
            continue;
        }
        uint32_t connector_id = 0;
        uint32_t crtc_id = 0;
        int crtc_index = -1;
        if (PickConnector(fd, width, height, &connector_id, &crtc_id, &crtc_index, mode) == 0 &&
            EnableAtomicCapabilities(fd) == 0) {
            display->drm_fd = fd;
            display->conn_id = connector_id;
            display->crtc_id = crtc_id;
            display->crtc_index = crtc_index;
            display->master_acquired = true;
            return 0;
        }
        close(fd);
    }
    return -1;
}

}  // namespace

int drm_display_init(DrmDisplay* display, int width, int height) {
    if (display == nullptr || width <= 0 || height <= 0) {
        return -1;
    }

    drm_mode_modeinfo mode{};
    if (OpenDisplayCard(display, width, height, &mode) != 0) {
        fprintf(stderr, "DRM: atomic KMS connector not available\n");
        return -1;
    }

    auto* state = new DrmAtomicState();
    state->mode = mode;
    display->mode_width = mode.hdisplay;
    display->mode_height = mode.vdisplay;
    display->ui_width = width;
    display->ui_height = height;
    display->atomic_state = state;
    fprintf(stderr, "DRM: UI framebuffer %dx%d -> display mode %dx%d%s%s\n",
            display->ui_width, display->ui_height,
            display->mode_width, display->mode_height,
            (mode.type & DRM_MODE_TYPE_PREFERRED) != 0 ? " (preferred)" : "",
            (display->ui_width != display->mode_width ||
             display->ui_height != display->mode_height) ? ", VOP plane scaling" : "");

    auto* original = new DrmCrtcState();
    original->conn_id = display->conn_id;
    original->valid = GetCrtc(display->drm_fd, display->crtc_id, &original->crtc) == 0;
    display->orig_crtc = original;

    if (ReadProperties(display->drm_fd, display->crtc_id, DRM_MODE_OBJECT_CRTC, &state->crtc) != 0 ||
        ReadProperties(display->drm_fd, display->conn_id, DRM_MODE_OBJECT_CONNECTOR, &state->connector) != 0 ||
        FindUiPlane(display->drm_fd, display->crtc_index, state) != 0 ||
        CreateUiBuffer(display->drm_fd, display->ui_width, display->ui_height, &state->ui_buffers[0]) != 0 ||
        CreateUiBuffer(display->drm_fd, display->ui_width, display->ui_height, &state->ui_buffers[1]) != 0) {
        fprintf(stderr, "DRM: NV12/ABGR dual plane setup failed\n");
        drm_display_deinit(display);
        return -1;
    }

    drm_mode_create_blob blob{};
    blob.length = sizeof(state->mode);
    blob.data = reinterpret_cast<uint64_t>(&state->mode);
    if (ioctl(display->drm_fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) != 0) {
        drm_display_deinit(display);
        return -1;
    }
    state->mode_blob_id = blob.blob_id;
    return 0;
}

int drm_display_get_ui_buffer(DrmDisplay* display, image_buffer_t* ui_buffer) {
    if (display == nullptr || ui_buffer == nullptr || display->atomic_state == nullptr) {
        return -1;
    }
    auto* state = static_cast<DrmAtomicState*>(display->atomic_state);
    DumbBuffer& buffer = state->ui_buffers[state->write_ui_index];
    if (buffer.dmabuf_fd < 0) {
        return -1;
    }
    memset(ui_buffer, 0, sizeof(*ui_buffer));
    ui_buffer->width = display->ui_width;
    ui_buffer->height = display->ui_height;
    ui_buffer->width_stride = buffer.pitch / 4;
    ui_buffer->height_stride = display->ui_height;
    ui_buffer->format = IMAGE_FORMAT_RGBA8888;
    ui_buffer->size = buffer.size;
    ui_buffer->fd = buffer.dmabuf_fd;
    return 0;
}

int drm_display_commit_ui(DrmDisplay* display) {
    if (display == nullptr || display->atomic_state == nullptr) {
        return -1;
    }
    auto* state = static_cast<DrmAtomicState*>(display->atomic_state);
    AtomicRequest request;
    if (!state->initialized) {
        AddModesetRequest(&request, *state, display);
    }
    const DumbBuffer& buffer = state->ui_buffers[state->write_ui_index];
    AddPlaneRequest(&request, state->ui_plane, display->crtc_id, buffer.fb_id,
                    display->ui_width, display->ui_height,
                    display->mode_width, display->mode_height, 1);
    const uint32_t flags = state->initialized ? 0 : DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (CommitAtomic(display->drm_fd, request, flags) != 0) {
        if (!state->ui_commit_error_reported) {
            fprintf(stderr, "DRM: atomic UI commit failed: %s\n", strerror(errno));
            state->ui_commit_error_reported = true;
        }
        return -1;
    }
    state->ui_commit_error_reported = false;
    state->current_ui_index = state->write_ui_index;
    state->write_ui_index = 1 - state->current_ui_index;
    state->initialized = true;
    return 0;
}

int drm_display_present(DrmDisplay* display, const image_buffer_t* image) {
    if (display == nullptr || image == nullptr || display->atomic_state == nullptr) {
        return -1;
    }
    image_buffer_t ui_buffer;
    if (drm_display_get_ui_buffer(display, &ui_buffer) != 0) {
        return -1;
    }
    image_buffer_t source = *image;
    if (convert_image(&source, &ui_buffer, nullptr, nullptr, 0) != 0) {
        return -1;
    }

    return drm_display_commit_ui(display);
}

int drm_display_present_nv12(DrmDisplay* display, const image_buffer_t* video_buffer) {
    if (display == nullptr || video_buffer == nullptr || display->atomic_state == nullptr) {
        return -1;
    }
    auto* state = static_cast<DrmAtomicState*>(display->atomic_state);
    const uint32_t fourcc = GetVideoFourcc(video_buffer);
    if (fourcc == 0) {
        return -1;
    }
    if (state->video_plane.id == 0 && FindVideoPlane(display->drm_fd, display->crtc_index, fourcc, state) != 0) {
        fprintf(stderr, "DRM: no video plane supports the MPP format\n");
        return -1;
    }
    uint32_t video_fb_id = 0;
    if (GetVideoFramebuffer(display->drm_fd, state, video_buffer, &video_fb_id) != 0) {
        fprintf(stderr, "DRM: import MPP NV12 DMA-BUF failed: %s\n", strerror(errno));
        return -1;
    }

    AtomicRequest request;
    const DumbBuffer& ui_buffer = state->ui_buffers[state->write_ui_index];
    if (!state->initialized) {
        AddModesetRequest(&request, *state, display);
    }
    AddPlaneRequest(&request, state->video_plane, display->crtc_id, video_fb_id,
                    video_buffer->width, video_buffer->height,
                    display->mode_width, display->mode_height, 0);
    AddPlaneRequest(&request, state->ui_plane, display->crtc_id, ui_buffer.fb_id,
                    display->ui_width, display->ui_height,
                    display->mode_width, display->mode_height, 1);

    const uint32_t flags = state->initialized ? 0 : DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (CommitAtomic(display->drm_fd, request, flags) != 0) {
        fprintf(stderr, "DRM: atomic dual-plane commit failed: %s\n", strerror(errno));
        return -1;
    }

    PurgeVideoFramebuffersWithOtherLayouts(display->drm_fd, state, video_buffer);
    state->current_ui_index = state->write_ui_index;
    state->write_ui_index = 1 - state->current_ui_index;
    state->initialized = true;
    return 0;
}

void drm_display_deinit(DrmDisplay* display) {
    if (display == nullptr || display->drm_fd < 0) {
        return;
    }

    if (display->orig_crtc != nullptr) {
        auto* original = static_cast<DrmCrtcState*>(display->orig_crtc);
        if (original->valid) {
            drm_mode_crtc restore = original->crtc;
            restore.set_connectors_ptr = reinterpret_cast<uint64_t>(&original->conn_id);
            restore.count_connectors = 1;
            ioctl(display->drm_fd, DRM_IOCTL_MODE_SETCRTC, &restore);
        }
        delete original;
        display->orig_crtc = nullptr;
    }

    if (display->atomic_state != nullptr) {
        auto* state = static_cast<DrmAtomicState*>(display->atomic_state);
        DestroyVideoFramebuffers(display->drm_fd, state);
        DestroyUiBuffer(display->drm_fd, &state->ui_buffers[0]);
        DestroyUiBuffer(display->drm_fd, &state->ui_buffers[1]);
        if (state->mode_blob_id != 0) {
            drm_mode_destroy_blob blob{};
            blob.blob_id = state->mode_blob_id;
            ioctl(display->drm_fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &blob);
        }
        delete state;
        display->atomic_state = nullptr;
    }

    if (display->master_acquired) {
        ioctl(display->drm_fd, DRM_IOCTL_DROP_MASTER, 0);
        display->master_acquired = false;
    }
    close(display->drm_fd);
    display->drm_fd = -1;
    display->mode_width = 0;
    display->mode_height = 0;
    display->ui_width = 0;
    display->ui_height = 0;
}
