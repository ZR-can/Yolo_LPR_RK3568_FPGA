#include <stdio.h>
#include <sys/time.h>

#include "mpp_decoder.h"
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

#ifdef MPP_DECODER_DEBUG
#define LOGD(...) printf(__VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

static const unsigned long kInputRetrySleepUs = 1000;
static const unsigned long kInputStallWarningMs = 1000;
static const unsigned long kInputStallAbortMs = 5000;
static const unsigned long kEosDrainLimitMs = 1000;

static unsigned long GetCurrentTimeMS() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec*1000+tv.tv_usec/1000;
}

MppDecoder::MppDecoder()
{

}

MppDecoder::~MppDecoder() {
    if (packet) {
        mpp_packet_deinit(&packet);
        packet = NULL;
    }
    if (loop_data.packet) {
        mpp_packet_deinit(&loop_data.packet);
        loop_data.packet = NULL;
    }
    if (frame) {
        mpp_frame_deinit(&frame);
        frame = NULL;
    }
    if (mpp_ctx) {
        mpp_destroy(mpp_ctx);
        mpp_ctx = NULL;
        loop_data.ctx = NULL;
        loop_data.mpi = NULL;
        mpp_mpi = NULL;
    }

    if (loop_data.frm_grp) {
        mpp_buffer_group_put(loop_data.frm_grp);
        loop_data.frm_grp = NULL;
    }
}

int MppDecoder::Init(int video_type, int fps, void* userdata)
{
    MPP_RET ret         = MPP_OK;
    this->userdata = userdata;
    this->fps = fps;
    this->last_frame_time_ms = 0;
    memset(&this->stats, 0, sizeof(this->stats));
    if(video_type == 264) {
        mpp_type  = MPP_VIDEO_CodingAVC;
    } else if (video_type == 265) {
        mpp_type  =MPP_VIDEO_CodingHEVC;
    } else {
        LOGD("unsupport video_type %d", video_type);
        return -1;
    }
    LOGD("mpi_dec_test start ");
    memset(&loop_data, 0, sizeof(loop_data));
    LOGD("mpi_dec_test decoder test start mpp_type %d ", mpp_type);

    MppDecCfg cfg       = NULL;

    MppCtx decoder_ctx = NULL;
    ret = mpp_create(&decoder_ctx, &mpp_mpi);
    if (MPP_OK != ret) {
        LOGD("mpp_create failed ");
        mpp_mpi = NULL;
        return -1;
    }
    this->mpp_ctx = decoder_ctx;

    auto cleanup_init = [&]() {
        if (cfg != NULL) {
            mpp_dec_cfg_deinit(cfg);
            cfg = NULL;
        }
        if (this->mpp_ctx != NULL) {
            mpp_destroy(this->mpp_ctx);
            this->mpp_ctx = NULL;
        }
        this->mpp_mpi = NULL;
        memset(&loop_data, 0, sizeof(loop_data));
    };

    RK_U32 parser_split_mode = need_split;
    ret = mpp_mpi->control(decoder_ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &parser_split_mode);
    if (ret) {
        fprintf(stderr, "MPP_DEC_SET_PARSER_SPLIT_MODE failed: %d\n", ret);
    }

    RK_U32 parser_fast_mode = 1;
    ret = mpp_mpi->control(decoder_ctx, MPP_DEC_SET_PARSER_FAST_MODE, &parser_fast_mode);
    if (ret) {
        fprintf(stderr, "MPP_DEC_SET_PARSER_FAST_MODE failed: %d\n", ret);
    }

    ret = mpp_init(decoder_ctx, MPP_CTX_DEC, mpp_type);
    if (ret) {
        LOGD("%p mpp_init failed ", decoder_ctx);
        cleanup_init();
        return -1;
    }

    mpp_dec_cfg_init(&cfg);

    /* get default config from decoder context */
    ret = mpp_mpi->control(decoder_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret) {
        LOGD("%p failed to get decoder cfg ret %d ", decoder_ctx, ret);
        cleanup_init();
        return -1;
    }

    /*
     * split_parse is to enable mpp internal frame spliter when the input
     * packet is not aplited into frames.
     */
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret) {
        LOGD("%p failed to set split_parse ret %d ", decoder_ctx, ret);
        cleanup_init();
        return -1;
    }

    ret = mpp_mpi->control(decoder_ctx, MPP_DEC_SET_CFG, cfg);
    if (ret) {
        LOGD("%p failed to set cfg %p ret %d ", decoder_ctx, cfg, ret);
        cleanup_init();
        return -1;
    }

    mpp_dec_cfg_deinit(cfg);
    cfg = NULL;

    RK_S64 timeout = MPP_TIMEOUT_NON_BLOCK;
    ret = mpp_mpi->control(decoder_ctx, MPP_SET_INPUT_TIMEOUT, &timeout);
    if (ret) {
        fprintf(stderr, "MPP_SET_INPUT_TIMEOUT failed: %d\n", ret);
    }
    ret = mpp_mpi->control(decoder_ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (ret) {
        fprintf(stderr, "MPP_SET_OUTPUT_TIMEOUT failed: %d\n", ret);
    }

    loop_data.ctx            = decoder_ctx;
    loop_data.mpi            = mpp_mpi;
    loop_data.eos            = 0;
    loop_data.packet_size    = packet_size;
    loop_data.frame          = 0;
    loop_data.frame_count    = 0;
    return 1;
}

int MppDecoder::Reset() {
    if (mpp_mpi != NULL && mpp_ctx != NULL) {
        return mpp_mpi->reset(mpp_ctx);
    }
    return -1;
}

MppDecoderStats MppDecoder::GetStats() const {
    MppDecoderStats result = stats;
    result.max_buffer_group_usage = loop_data.max_usage;
    return result;
}

int MppDecoder::Decode(uint8_t* pkt_data, int pkt_size, int pkt_eos)
{
    MpiDecLoopData *data=&loop_data;
    RK_U32 pkt_done = 0;
    RK_U32 err_info = 0;
    MPP_RET ret = MPP_OK;
    MPP_RET fatal_ret = MPP_OK;
    MppCtx ctx  = data->ctx;
    MppApi *mpi = data->mpi;
    unsigned long input_stall_begin_ms = 0;
    unsigned long input_retry_streak = 0;
    bool input_stall_reported = false;
    unsigned long eos_wait_begin_ms = 0;
    bool eos_seen = false;

    size_t read_size = 0;
    size_t packet_size = data->packet_size;

    LOGD("receive packet size=%d ", pkt_size);

    if (packet == NULL) {
        ret = mpp_packet_init(&packet, NULL, 0);
        if (ret != MPP_OK) {
            return ret;
        }
    }

    ///////////////////////////////////////////////
    // ret = mpp_packet_init(&packet, frame_data, frame_size);
    mpp_packet_set_data(packet, pkt_data);
    mpp_packet_set_size(packet, pkt_size);
    mpp_packet_set_pos(packet, pkt_data);
    mpp_packet_set_length(packet, pkt_size);
    // setup eos flag
    if (pkt_eos)
        mpp_packet_set_eos(packet);
    do {

        RK_S32 times = 5;
        bool decoded_frame = false;
        // send the packet first if packet is not done
        if (!pkt_done) {
            unsigned long put_begin_ms = GetCurrentTimeMS();
            ret = mpi->decode_put_packet(ctx, packet);
            stats.put_packet_time_ms += GetCurrentTimeMS() - put_begin_ms;
            if (MPP_OK == ret) {
                pkt_done = 1;
                input_stall_begin_ms = 0;
                input_retry_streak = 0;
                input_stall_reported = false;
            } else if (ret == MPP_ERR_BUFFER_FULL || ret == MPP_ERR_TIMEOUT) {
                stats.input_retry_count++;
                if (ret == MPP_ERR_BUFFER_FULL) {
                    stats.input_buffer_full_count++;
                } else {
                    stats.input_timeout_count++;
                }
            } else {
                stats.input_fatal_error_count++;
                fprintf(stderr, "decode_put_packet failed: %d\n", ret);
                mpp_packet_deinit(&packet);
                return ret;
            }
        }
        // then get all available frame and release
        do {
            RK_S32 get_frm = 0;
            RK_U32 frm_eos = 0;

            try_again:
            unsigned long get_begin_ms = GetCurrentTimeMS();
            ret = mpi->decode_get_frame(ctx, &frame);
            stats.get_frame_time_ms += GetCurrentTimeMS() - get_begin_ms;
            if (MPP_ERR_TIMEOUT == ret) {
                if (times > 0) {
                    times--;
                    stats.timeout_sleep_ms += 2;
                    usleep(2000);
                    goto try_again;
                }
                LOGD("decode_get_frame failed too much time ");
            }

            if (MPP_OK != ret) {
                LOGD("decode_get_frame failed ret %d ", ret);
                if (ret != MPP_ERR_TIMEOUT) {
                    stats.output_error_count++;
                    fatal_ret = ret;
                }
                break;
            }

            if (frame) {
                RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                RK_U32 hor_width = mpp_frame_get_width(frame);
                RK_U32 ver_height = mpp_frame_get_height(frame);
                RK_U32 buf_size = mpp_frame_get_buf_size(frame);
                RK_S64 pts = mpp_frame_get_pts(frame);
                RK_S64 dts = mpp_frame_get_dts(frame);

                LOGD("decoder require buffer w:h [%d:%d] stride [%d:%d] buf_size %d pts=%lld dts=%lld ",
                        hor_width, ver_height, hor_stride, ver_stride, buf_size, pts, dts);

                if (mpp_frame_get_info_change(frame)) {

                    LOGD("decode_get_frame get info changed found ");
                    // ret = mpp_buffer_group_get_internal(&data->frm_grp, MPP_BUFFER_TYPE_DRM);
                    // if (ret) {
                    //     LOGD("get mpp buffer group  failed ret %d ", ret);
                    //     break;
                    // }
                    // mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);
                    // mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (NULL == data->frm_grp) {
                        /* If buffer group is not set create one and limit it */
                        ret = mpp_buffer_group_get_internal(&data->frm_grp, MPP_BUFFER_TYPE_DRM);
                        if (ret) {
                            LOGD("%p get mpp buffer group failed ret %d ", ctx, ret);
                            break;
                        }

                        /* Set buffer to mpp decoder */
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);
                        if (ret) {
                            LOGD("%p set buffer group failed ret %d ", ctx, ret);
                            break;
                        }
                    } else {
                        /* If old buffer group exist clear it */
                        ret = mpp_buffer_group_clear(data->frm_grp);
                        if (ret) {
                            LOGD("%p clear buffer group failed ret %d ", ctx, ret);
                            break;
                        }
                    }

                    /* Use limit config to limit buffer count to 24 with buf_size */
                    ret = mpp_buffer_group_limit_config(data->frm_grp, buf_size, 24);
                    if (ret) {
                        LOGD("%p limit buffer group failed ret %d ", ctx, ret);
                        break;
                    }

                    /*
                     * All buffer group config done. Set info change ready to let
                     * decoder continue decoding
                     */
                    ret = mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret) {
                        LOGD("%p info change ready failed ret %d ", ctx, ret);
                        break;
                    }

                    this->last_frame_time_ms = GetCurrentTimeMS();
                } else {
                    err_info = mpp_frame_get_errinfo(frame) | mpp_frame_get_discard(frame);
                    if (err_info) {
                        LOGD("decoder_get_frame get err info:%d discard:%d. ",
                                mpp_frame_get_errinfo(frame), mpp_frame_get_discard(frame));
                    }
                    data->frame_count++;
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    LOGD("get one frame %ld ",  (tv.tv_sec * 1000 + tv.tv_usec/1000));
                    // mpp_frame_get_width(frame);
                    // char *input_data =(char *) mpp_buffer_get_ptr(mpp_frame_get_buffer(frame));
                    if (callback != nullptr) {
                        MppBuffer buffer = mpp_frame_get_buffer(frame);
                        MppFrameFormat format = mpp_frame_get_fmt(frame);
                        char *data_vir =(char *) mpp_buffer_get_ptr(buffer);
                        int fd = mpp_buffer_get_fd(buffer);
                        LOGD("data_vir=%p fd=%d ", data_vir, fd);
                        if (mpp_buffer_inc_ref(buffer) == MPP_OK) {
                            unsigned long callback_begin_ms = GetCurrentTimeMS();
                            callback(this->userdata, hor_stride, ver_stride, hor_width, ver_height,
                                     format, fd, data_vir, buffer);
                            stats.callback_count++;
                            stats.callback_time_ms += GetCurrentTimeMS() - callback_begin_ms;
                        } else {
                            LOGD("mpp_buffer_inc_ref failed for display callback");
                        }
                    }
                    unsigned long cur_time_ms = GetCurrentTimeMS();
                    long time_gap = 1000/this->fps - (cur_time_ms - this->last_frame_time_ms);
                    LOGD("time_gap=%ld", time_gap);
                    if (time_gap > 0) {
                        stats.pacing_sleep_ms += time_gap;
                        usleep(time_gap * 1000);
                    }
                    this->last_frame_time_ms = GetCurrentTimeMS();
                }
                frm_eos = mpp_frame_get_eos(frame);

                ret = mpp_frame_deinit(&frame);
                frame = NULL;

                // if(frame_pre!=NULL)
                // {
                //     mpp_frame_deinit(&frame_pre);
                // }
                // &frame_pre=&frame;

                get_frm = 1;
                decoded_frame = true;
            }

            // try get runtime frame memory usage
            if (data->frm_grp) {
                size_t usage = mpp_buffer_group_usage(data->frm_grp);
                if (usage > data->max_usage)
                    data->max_usage = usage;
            }

            // if last packet is send but last frame is not found continue
            if (pkt_eos && pkt_done && !frm_eos) {
                usleep(1*1000);
                continue;
            }

            if (frm_eos) {
                LOGD("found last frame ");
                eos_seen = true;
                break;
            }

            if (data->frame_num > 0 && data->frame_count >= data->frame_num) {
                data->eos = 1;
                break;
            }

            if (get_frm)
                continue;
            break;
        } while (1);

        if (fatal_ret != MPP_OK) {
            break;
        }

        if (ret != MPP_OK && ret != MPP_ERR_TIMEOUT) {
            stats.output_error_count++;
            fatal_ret = ret;
            break;
        }

        if (pkt_eos && pkt_done && !eos_seen) {
            if (eos_wait_begin_ms == 0) {
                eos_wait_begin_ms = GetCurrentTimeMS();
            }
            if (GetCurrentTimeMS() - eos_wait_begin_ms >= kEosDrainLimitMs) {
                stats.output_error_count++;
                fprintf(stderr, "MPP EOS drain timed out after %lu ms\n",
                        GetCurrentTimeMS() - eos_wait_begin_ms);
                fatal_ret = MPP_ERR_TIMEOUT;
                break;
            }
            usleep(kInputRetrySleepUs);
            continue;
        }

        if (data->frame_num > 0 && data->frame_count >= data->frame_num) {
            data->eos = 1;
            LOGD("reach max frame number %d ", data->frame_count);
            break;
        }

        if (pkt_done)
            break;

        if (decoded_frame) {
            input_stall_begin_ms = 0;
            input_retry_streak = 0;
            input_stall_reported = false;
            continue;
        }

        input_retry_streak++;
        if (input_retry_streak > stats.max_input_retry_streak) {
            stats.max_input_retry_streak = input_retry_streak;
        }

        if (input_stall_begin_ms == 0) {
            input_stall_begin_ms = GetCurrentTimeMS();
        }
        unsigned long input_stall_elapsed_ms = GetCurrentTimeMS() - input_stall_begin_ms;
        if (!input_stall_reported && input_stall_elapsed_ms >= kInputStallWarningMs) {
            stats.input_stall_count++;
            input_stall_reported = true;
            fprintf(stderr, "MPP input backlog for %lu ms after %lu retries; continuing\n",
                    input_stall_elapsed_ms, input_retry_streak);
        }
        if (input_stall_elapsed_ms >= kInputStallAbortMs) {
            stats.input_abort_count++;
            fprintf(stderr, "MPP input stalled for %lu ms after %lu retries\n",
                    input_stall_elapsed_ms, input_retry_streak);
            ret = MPP_ERR_TIMEOUT;
            break;
        }

        unsigned long retry_sleep_begin_ms = GetCurrentTimeMS();
        usleep(kInputRetrySleepUs);
        stats.input_retry_sleep_ms += GetCurrentTimeMS() - retry_sleep_begin_ms;
    } while (1);
    mpp_packet_deinit(&packet);

    if (fatal_ret != MPP_OK) {
        return fatal_ret;
    }
    return pkt_done ? MPP_OK : ret;
}

int MppDecoder::SetCallback(MppDecoderFrameCallback callback) {
    this->callback = callback;
    return 0;
}
