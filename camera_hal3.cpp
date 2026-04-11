/*
 * Minimal Camera HAL v3 for Tegra K1 (Xiaomi Mi Pad mocha).
 *
 * Uses NvCameraCore API from libnvmm_camera_v3.so (dlopen).
 * Based on JXD camera_v3 HAL source — simplified to minimum for preview.
 */

#define LOG_TAG "CameraHAL3"

#include <stdio.h>
#include <cutils/log.h>
#include <hardware/camera3.h>
#include <hardware/camera_common.h>
#include <hardware/hardware.h>
#include <system/camera_metadata.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvcamera_core_api.h"

#include "miui_camera_metadata_tags.h"

/* Pixel formats & gralloc usage */
#define HAL_PIXEL_FORMAT_YCrCb_420_SP       0x11
#define HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED 0x22
#define GRALLOC_USAGE_HW_CAMERA_WRITE       0x00020000
#define GRALLOC_USAGE_HW_CAMERA_READ        0x00010000

/* File-based log (logcat overflows with NvOsDebugPrintf spam) */
static FILE *g_logf;
#define FLOG(...) do { if (g_logf) { fprintf(g_logf, __VA_ARGS__); fflush(g_logf); } } while(0)

/* -------------------------------------------------------------------------- */
/* NvCameraCore API + nvgr — loaded at runtime                                */
/* -------------------------------------------------------------------------- */

static void *g_nvmm_lib;
static void *g_nvgr_lib;

static pfn_NvCameraCore_Open                    fn_Open;
static pfn_NvCameraCore_Close                   fn_Close;
static pfn_NvCameraCore_CallbackFunction        fn_Callback;
static pfn_NvCameraCore_SetSensorMode           fn_SetSensorMode;
static pfn_NvCameraCore_FrameCaptureRequest     fn_FrameCaptureRequest;
static pfn_NvCameraCore_Flush                   fn_Flush;
static pfn_NvCameraCore_GetDefaultControlProperties fn_GetDefaultCtrl;
static pfn_NvMMCameraDeviceDetect               fn_DeviceDetect;
static pfn_nvgr_get_surfaces                    fn_nvgr_get_surfaces;

/* camera_metadata function pointers */
typedef camera_metadata_t *(*pfn_allocate_camera_metadata)(size_t, size_t);
typedef int (*pfn_add_camera_metadata_entry)(camera_metadata_t*, uint32_t, const void*, size_t);

static pfn_allocate_camera_metadata fn_alloc_meta;
static pfn_add_camera_metadata_entry fn_add_meta;

static int load_libs(void)
{
    if (g_nvmm_lib) return 0;

    g_nvmm_lib = dlopen("libnvmm_camera_v3.so", RTLD_NOW);
    if (!g_nvmm_lib) { ALOGE("dlopen libnvmm_camera_v3.so: %s", dlerror()); return -1; }

#define LSYM(var, lib, name) var = (__typeof__(var))dlsym(lib, name); \
    if (!var) ALOGW("missing: %s", name);

    LSYM(fn_Open, g_nvmm_lib, "NvCameraCore_Open");
    LSYM(fn_Close, g_nvmm_lib, "NvCameraCore_Close");
    LSYM(fn_Callback, g_nvmm_lib, "NvCameraCore_CallbackFunction");
    LSYM(fn_SetSensorMode, g_nvmm_lib, "NvCameraCore_SetSensorMode");
    LSYM(fn_FrameCaptureRequest, g_nvmm_lib, "NvCameraCore_FrameCaptureRequest");
    LSYM(fn_Flush, g_nvmm_lib, "NvCameraCore_Flush");
    LSYM(fn_GetDefaultCtrl, g_nvmm_lib, "NvCameraCore_GetDefaultControlProperties");
    LSYM(fn_DeviceDetect, g_nvmm_lib, "NvMMCameraDeviceDetect");

    g_nvgr_lib = dlopen("libnvgr.so", RTLD_NOW);
    if (g_nvgr_lib) LSYM(fn_nvgr_get_surfaces, g_nvgr_lib, "nvgr_get_surfaces");

    /* camera_metadata from libcamera_metadata.so */
    void *meta_lib = dlopen("libcamera_metadata.so", RTLD_NOW);
    if (meta_lib) {
        LSYM(fn_alloc_meta, meta_lib, "allocate_camera_metadata");
        LSYM(fn_add_meta, meta_lib, "add_camera_metadata_entry");
    }

#undef LSYM

    if (!fn_Open || !fn_Close || !fn_FrameCaptureRequest) return -1;

    ALOGI("NvCameraCore loaded (nvgr=%d meta=%d)", !!fn_nvgr_get_surfaces, !!fn_alloc_meta);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Minimal camera_metadata builders                                            */
/* -------------------------------------------------------------------------- */

static camera_metadata_t *build_static_info(void)
{
    if (!fn_alloc_meta || !fn_add_meta) return NULL;

    camera_metadata_t *m = fn_alloc_meta(50, 2048);
    if (!m) return NULL;

    /* Minimal static characteristics for camera service to work */
    uint8_t facing = CAMERA_FACING_FRONT;
    int32_t orientation = 270;
    int32_t max_output_streams[] = {0, 1, 0}; /* raw, processed, jpeg */
    int32_t active_array[] = {0, 0, 2592, 1944};
    int32_t pixel_array[] = {2592, 1944};
    int32_t avail_formats[] = {HAL_PIXEL_FORMAT_YCrCb_420_SP,
                               HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED};
    int32_t avail_stream_configs[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 2592, 1944, 0, /* OUTPUT */
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, 0,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 0,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 640, 480, 0,
        HAL_PIXEL_FORMAT_YCrCb_420_SP, 2592, 1944, 0,
        HAL_PIXEL_FORMAT_YCrCb_420_SP, 1920, 1080, 0,
        HAL_PIXEL_FORMAT_YCrCb_420_SP, 640, 480, 0,
    };
    int64_t min_frame_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 2592, 1944, 33333333LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 640, 480, 33333333LL,
    };
    int64_t stall_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 2592, 1944, 0LL,
    };
    uint8_t hw_level = 2; /* FULL */
    int32_t max_regions[] = {0, 0, 0}; /* AE, AWB, AF */
    uint8_t avail_caps[] = {0}; /* BACKWARD_COMPATIBLE */
    int32_t request_keys[] = {0};
    int32_t result_keys[] = {0};
    int32_t chars_keys[] = {0};

    /* AE fps ranges — required by Camera2-Parameters */
    int32_t ae_fps_ranges[] = {15, 30};
    int32_t ae_compensation_range[] = {-4, 4};
    float ae_compensation_step[] = {0.5f}; /* rational as float */
    uint8_t ae_available_modes[] = {0, 1}; /* OFF, ON */
    uint8_t awb_available_modes[] = {0, 1}; /* OFF, AUTO */
    uint8_t af_available_modes[] = {0}; /* OFF */
    uint8_t avail_effects[] = {0}; /* OFF */
    uint8_t avail_scene_modes[] = {0}; /* DISABLED */
    uint8_t avail_antibanding[] = {0}; /* OFF */
    float focal_lengths[] = {3.5f};
    float apertures[] = {2.0f};
    int32_t max_face_count[] = {0};
    int64_t exposure_range[] = {1000000LL, 300000000LL}; /* 1ms - 300ms */
    int32_t sensitivity_range[] = {100, 1600};

    fn_add_meta(m, MIUI_LENS_FACING, &facing, 1);
    fn_add_meta(m, MIUI_SENSOR_ORIENTATION, &orientation, 1);
    fn_add_meta(m, MIUI_REQUEST_MAX_NUM_OUTPUT_STREAMS, max_output_streams, 3);
    fn_add_meta(m, MIUI_SENSOR_INFO_ACTIVE_ARRAY, active_array, 4);
    fn_add_meta(m, MIUI_SENSOR_INFO_PIXEL_ARRAY, pixel_array, 2);
    fn_add_meta(m, MIUI_SCALER_AVAIL_FORMATS, avail_formats, 2);
    fn_add_meta(m, MIUI_CONTROL_MAX_REGIONS, max_regions, 3);
    fn_add_meta(m, MIUI_CONTROL_AE_AVAIL_FPS_RANGES, ae_fps_ranges, 2);
    fn_add_meta(m, MIUI_CONTROL_AE_COMP_RANGE, ae_compensation_range, 2);
    fn_add_meta(m, MIUI_CONTROL_AE_COMP_STEP, ae_compensation_step, 1);
    fn_add_meta(m, MIUI_CONTROL_AE_AVAIL_MODES, ae_available_modes, 2);
    fn_add_meta(m, MIUI_CONTROL_AWB_AVAIL_MODES, awb_available_modes, 2);
    fn_add_meta(m, MIUI_CONTROL_AF_AVAIL_MODES, af_available_modes, 1);
    fn_add_meta(m, MIUI_CONTROL_AVAIL_EFFECTS, avail_effects, 1);
    fn_add_meta(m, MIUI_CONTROL_AVAIL_SCENE_MODES, avail_scene_modes, 1);
    fn_add_meta(m, MIUI_CONTROL_AE_AVAIL_ANTIBANDING, avail_antibanding, 1);
    fn_add_meta(m, MIUI_LENS_INFO_AVAIL_FOCAL_LENGTHS, focal_lengths, 1);
    fn_add_meta(m, MIUI_LENS_INFO_AVAIL_APERTURES, apertures, 1);
    fn_add_meta(m, MIUI_STATS_INFO_MAX_FACE_COUNT, max_face_count, 1);
    fn_add_meta(m, MIUI_SENSOR_INFO_EXPOSURE_RANGE, exposure_range, 2);
    fn_add_meta(m, MIUI_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity_range, 2);

    /* Scaler: processed sizes, JPEG sizes, min durations */
    int32_t processed_sizes[] = {2592, 1944, 1920, 1080, 1280, 720, 640, 480};
    int32_t jpeg_sizes[] = {2592, 1944, 1920, 1080};
    int64_t processed_min_dur[] = {33333333LL, 33333333LL, 33333333LL, 33333333LL};
    int64_t jpeg_min_dur[] = {33333333LL, 33333333LL};

    fn_add_meta(m, MIUI_SCALER_AVAIL_PROC_SIZES, processed_sizes, 8);
    fn_add_meta(m, MIUI_SCALER_AVAIL_JPEG_SIZES, jpeg_sizes, 4);
    fn_add_meta(m, MIUI_SCALER_AVAIL_PROC_MIN_DUR, processed_min_dur, 4);
    fn_add_meta(m, MIUI_SCALER_AVAIL_JPEG_MIN_DUR, jpeg_min_dur, 2);

    /* JPEG thumbnail sizes */
    int32_t thumb_sizes[] = {0, 0, 160, 120, 320, 240};
    fn_add_meta(m, MIUI_JPEG_AVAIL_THUMB_SIZES, thumb_sizes, 6);

    /* Flash */
    uint8_t flash_available = 0;
    fn_add_meta(m, MIUI_FLASH_INFO_AVAILABLE, &flash_available, 1);

    /* Video stabilization */
    uint8_t vstab_modes[] = {0}; /* OFF */
    fn_add_meta(m, MIUI_CONTROL_AVAIL_VSTAB_MODES, vstab_modes, 1);

    /* Max digital zoom */
    float max_zoom = 1.0f;
    fn_add_meta(m, MIUI_SCALER_AVAIL_MAX_DIGITAL_ZOOM, &max_zoom, 1);

    /* Physical sensor size (mm) — OV5693 1/4" approx */
    float physical_size[] = {3.67f, 2.74f};
    fn_add_meta(m, MIUI_SENSOR_INFO_PHYSICAL_SIZE, physical_size, 2);

    /* Face detect */
    uint8_t face_detect_modes[] = {0}; /* OFF */
    fn_add_meta(m, MIUI_STATS_INFO_AVAIL_FACE_DETECT, face_detect_modes, 1);

    /* Lens */
    float min_focus_dist = 0.0f; /* fixed focus */
    fn_add_meta(m, MIUI_LENS_INFO_MIN_FOCUS_DISTANCE, &min_focus_dist, 1);

    /* JPEG max size */
    int32_t jpeg_max_size = 2592 * 1944 * 3 / 2 + 65536; /* NV12 + overhead */
    fn_add_meta(m, MIUI_JPEG_MAX_SIZE, &jpeg_max_size, 1);

    /* Hardware level */
    fn_add_meta(m, MIUI_INFO_HW_LEVEL, &hw_level, 1);

    (void)avail_stream_configs; (void)min_frame_durations;
    (void)stall_durations; (void)avail_caps;
    (void)request_keys; (void)result_keys; (void)chars_keys;

    /*
     * MIUI CameraService stubs.
     *
     * Xiaomi's libcameraservice.so has custom code paths (e.g. setTimestampMultFactor)
     * that call CameraMetadata::find() on tags not present in AOSP. If the tag is
     * missing, find() returns NULL and the service crashes (SEGV).
     *
     * These stubs provide safe defaults. They are NOT part of the core HAL and
     * should be removed or ifdef'd out when targeting AOSP/LineageOS CameraService.
     */
#ifdef MIUI_CAMERA_SERVICE
    int64_t max_frame_dur = 300000000LL; /* 300ms = ~3fps min */
    fn_add_meta(m, MIUI_SENSOR_INFO_MAX_FRAME_DUR, &max_frame_dur, 1);

    int32_t white_level = 4095; /* 12-bit */
    fn_add_meta(m, MIUI_SENSOR_INFO_WHITE_LEVEL, &white_level, 1);
#endif

    return m;
}

static camera_metadata_t *build_default_request(void)
{
    if (!fn_alloc_meta || !fn_add_meta) return NULL;

    camera_metadata_t *m = fn_alloc_meta(10, 128);
    if (!m) return NULL;

    uint8_t intent = 1; /* PREVIEW */
    uint8_t mode = 1;   /* AUTO */
    uint8_t ae_mode = 1; /* ON */
    uint8_t awb_mode = 1; /* AUTO */

    fn_add_meta(m, MIUI_CONTROL_CAPTURE_INTENT, &intent, 1);
    fn_add_meta(m, MIUI_CONTROL_MODE, &mode, 1);
    fn_add_meta(m, MIUI_CONTROL_AE_MODE, &ae_mode, 1);
    fn_add_meta(m, MIUI_CONTROL_AWB_MODE, &awb_mode, 1);

    return m;
}

static camera_metadata_t *build_result_meta(uint32_t frame_number)
{
    if (!fn_alloc_meta || !fn_add_meta) return NULL;

    camera_metadata_t *m = fn_alloc_meta(5, 64);
    if (!m) return NULL;

    int64_t timestamp = frame_number * 33333333LL;
    uint8_t ae_state = 2; /* CONVERGED */
    uint8_t awb_state = 2;

    fn_add_meta(m, MIUI_SENSOR_TIMESTAMP, &timestamp, 1);
    fn_add_meta(m, MIUI_CONTROL_AE_STATE, &ae_state, 1);
    fn_add_meta(m, MIUI_CONTROL_AWB_STATE, &awb_state, 1);

    return m;
}

/* -------------------------------------------------------------------------- */
/* Per-camera context                                                          */
/* -------------------------------------------------------------------------- */

#define MAX_BUFFERS 8

struct stream_context {
    camera3_stream_t    *stream;
    NvMMBuffer           nvmm_bufs[MAX_BUFFERS];
    buffer_handle_t     *anb_handles[MAX_BUFFERS];
    int                  num_bufs;
};

struct camera_context {
    camera3_device_t             device;
    NvCameraCoreHandle           core_handle;
    int                          camera_id;
    const camera3_callback_ops_t *callback_ops;

    struct stream_context        stream;
    int                          stream_configured;

    /* Pending frame tracking */
    volatile uint32_t            pending_frame;
    volatile int                 frame_done;
    NvMMBuffer                  *completed_buffer;
};

/* -------------------------------------------------------------------------- */
/* NvCameraCore event callback                                                 */
/* -------------------------------------------------------------------------- */

static NvError nvcamera_callback(void *ctx_ptr, NvU32 event_type,
                                  NvU32 info_size, void *info)
{
    struct camera_context *ctx = (struct camera_context *)ctx_ptr;
    (void)info_size;

    if (event_type == NvCameraCoreEvent_CompletedBuffer) {
        NvCameraCoreFrameCaptureResult *result = (NvCameraCoreFrameCaptureResult *)info;
        FLOG("CompletedBuffer: frame %u, %u bufs\n",
             result->FrameNumber, result->NumCompletedOutputBuffers);

        if (result->ppOutputBuffers && result->NumCompletedOutputBuffers > 0)
            ctx->completed_buffer = result->ppOutputBuffers[0];

        ctx->frame_done = 1;
    } else if (event_type == NvCameraCoreEvent_Shutter) {
        NvCameraCoreShutterEventInfo *shutter = (NvCameraCoreShutterEventInfo *)info;
        /* Notify framework about shutter */
        if (ctx->callback_ops) {
            camera3_notify_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = CAMERA3_MSG_SHUTTER;
            msg.message.shutter.frame_number = shutter->FrameNumber;
            msg.message.shutter.timestamp = shutter->Timestamp;
            ctx->callback_ops->notify(ctx->callback_ops, &msg);
        }
    }

    return NvSuccess;
}

/* -------------------------------------------------------------------------- */
/* Buffer conversion: gralloc → NvMMBuffer (via nvgr_get_surfaces)             */
/* -------------------------------------------------------------------------- */

static int link_buffer(buffer_handle_t *buf, NvMMBuffer *nvmm, NvU32 buf_id)
{
    if (!fn_nvgr_get_surfaces || !buf || !*buf) return -1;

    const NvRmSurface *surfs = NULL;
    size_t surf_count = 0;
    fn_nvgr_get_surfaces((void *)*buf, &surfs, &surf_count);
    if (!surfs || surf_count == 0) return -1;

    memset(nvmm, 0, sizeof(*nvmm));
    nvmm->StructSize = sizeof(NvMMBuffer);
    nvmm->BufferID = buf_id;
    nvmm->PayloadType = NvMMPayloadType_SurfaceArray;

    NvMMSurfaceDescriptor *desc = &nvmm->Payload.Surfaces;
    memcpy(desc->Surfaces, surfs,
           sizeof(NvRmSurface) * (surf_count > 3 ? 3 : surf_count));
    desc->SurfaceCount = surf_count;
    desc->Empty = NV_TRUE;

    FLOG("link_buffer: id=%u surfs=%zu %ux%u fmt=0x%x pitch=%u hMem=%p\n",
         buf_id, surf_count, surfs[0].Width, surfs[0].Height,
         surfs[0].ColorFormat, surfs[0].Pitch, surfs[0].hMem);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* camera3_device_ops implementation                                           */
/* -------------------------------------------------------------------------- */

static int hal3_initialize(const camera3_device_t *dev,
                           const camera3_callback_ops_t *callback_ops)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct camera_context *ctx = (struct camera_context *)dev->priv;
    ctx->callback_ops = callback_ops;
    FLOG("initialize: callback_ops=%p\n", callback_ops);
    return 0;
}

static int hal3_configure_streams(const camera3_device_t *dev,
                                  camera3_stream_configuration_t *config)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct camera_context *ctx = (struct camera_context *)dev->priv;

    if (!config || config->num_streams == 0 || !config->streams) return -EINVAL;

    FLOG("configure_streams: num_streams=%d\n", config->num_streams);

    /* Configure ALL streams — set usage and max_buffers for each */
    camera3_stream_t *preview_stream = NULL;
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *s = config->streams[i];
        if (!s) continue;

        s->usage |= GRALLOC_USAGE_HW_CAMERA_WRITE;
        s->max_buffers = 4;

        FLOG("  stream[%d]: %dx%d fmt=0x%x type=%d\n",
             i, s->width, s->height, s->format, s->stream_type);

        /* Pick largest output stream for sensor mode */
        if (s->stream_type == CAMERA3_STREAM_OUTPUT ||
            s->stream_type == CAMERA3_STREAM_BIDIRECTIONAL) {
            if (!preview_stream ||
                (s->width * s->height > preview_stream->width * preview_stream->height))
                preview_stream = s;
        }
    }

    if (preview_stream) {
        ctx->stream.stream = preview_stream;
        ctx->stream.num_bufs = 0;
        ctx->stream_configured = 1;

        /* Set sensor mode to match largest stream */
        NvMMCameraSensorMode mode;
        memset(&mode, 0, sizeof(mode));
        mode.Resolution.width = preview_stream->width;
        mode.Resolution.height = preview_stream->height;
        mode.FrameRate = 30.0f;
        NvError err = fn_SetSensorMode(ctx->core_handle, mode);
        FLOG("SetSensorMode %dx%d -> %d\n",
             preview_stream->width, preview_stream->height, err);
    }

    return 0;
}

static int hal3_register_stream_buffers(const camera3_device_t *dev,
                                        const camera3_stream_buffer_set_t *buf_set)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct camera_context *ctx = (struct camera_context *)dev->priv;

    if (!buf_set || buf_set->num_buffers == 0 || !buf_set->buffers) return -EINVAL;

    FLOG("register_stream_buffers: %d buffers for stream %p\n",
         buf_set->num_buffers, buf_set->stream);

    /* Pre-link all buffers to NvMMBuffer */
    int n = buf_set->num_buffers > MAX_BUFFERS ? MAX_BUFFERS : buf_set->num_buffers;
    for (int i = 0; i < n; i++) {
        ctx->stream.anb_handles[i] = buf_set->buffers[i];
        link_buffer(buf_set->buffers[i], &ctx->stream.nvmm_bufs[i], i);
    }
    ctx->stream.num_bufs = n;

    return 0;
}

static const camera_metadata_t *hal3_construct_default_request_settings(
        const camera3_device_t *dev, int type)
{
    static camera_metadata_t *s_default = NULL;
    (void)dev; (void)type;

    if (!s_default) s_default = build_default_request();
    return s_default;
}

static int hal3_process_capture_request(const camera3_device_t *dev,
                                        camera3_capture_request_t *request)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct camera_context *ctx = (struct camera_context *)dev->priv;

    if (!request || request->num_output_buffers == 0 || !request->output_buffers) return -EINVAL;
    if (!ctx->callback_ops || !ctx->callback_ops->process_capture_result) return -EINVAL;

    uint32_t frame_num = request->frame_number;
    const camera3_stream_buffer_t *out_const = &request->output_buffers[0];
    camera3_stream_buffer_t out_copy = *out_const;

    /* Find or create NvMMBuffer for this gralloc handle */
    NvMMBuffer *nvmm = NULL;

    /* Check pre-linked buffers */
    for (int i = 0; i < ctx->stream.num_bufs; i++) {
        if (ctx->stream.anb_handles[i] == out_copy.buffer) {
            nvmm = &ctx->stream.nvmm_bufs[i];
            nvmm->Payload.Surfaces.Empty = NV_TRUE;
            break;
        }
    }

    /* Not pre-linked — link on the fly */
    if (!nvmm && ctx->stream.num_bufs < MAX_BUFFERS) {
        int idx = ctx->stream.num_bufs;
        ctx->stream.anb_handles[idx] = out_copy.buffer;
        if (link_buffer(out_copy.buffer, &ctx->stream.nvmm_bufs[idx], idx) == 0) {
            nvmm = &ctx->stream.nvmm_bufs[idx];
            ctx->stream.num_bufs++;
        }
    }

    if (!nvmm) {
        FLOG("process_capture_request: no NvMMBuffer for frame %u\n", frame_num);
        ALOGE("process_capture_request: no NvMMBuffer for frame %u", frame_num);
        return -ENOMEM;
    }

    /* Submit to NvCameraCore */
    NvMMBuffer *out_bufs[1] = {nvmm};
    NvCameraCoreFrameCaptureRequest req;
    memset(&req, 0, sizeof(req));
    req.FrameNumber = frame_num;
    req.NumOfOutputBuffers = 1;
    req.ppOutputBuffers = out_bufs;

    ctx->frame_done = 0;
    NvError err = fn_FrameCaptureRequest(ctx->core_handle, &req);

    if (frame_num < 5)
        FLOG("FrameCaptureRequest frame %u -> %d\n", frame_num, err);

    if (err != NvSuccess) {
        FLOG("FrameCaptureRequest FAILED: %d\n", err);
        return -EIO;
    }

    /* Wait for CompletedBuffer (max 500ms) */
    int wait_ms = 0;
    while (!ctx->frame_done && wait_ms < 500) {
        usleep(1000);
        wait_ms++;
    }

    if (frame_num < 5)
        FLOG("frame %u done=%d wait=%dms\n", frame_num, ctx->frame_done, wait_ms);

    /* Return result to framework */
    camera3_capture_result_t result;
    memset(&result, 0, sizeof(result));
    result.frame_number = frame_num;
    result.num_output_buffers = 1;

    camera3_stream_buffer_t result_buf = out_copy;
    result_buf.status = ctx->frame_done ? CAMERA3_BUFFER_STATUS_OK : CAMERA3_BUFFER_STATUS_ERROR;
    result_buf.release_fence = -1;
    result.output_buffers = &result_buf;

    result.result = build_result_meta(frame_num);

    ctx->callback_ops->process_capture_result(ctx->callback_ops, &result);

    /* Free result metadata */
    if (result.result) free((void *)result.result);

    return 0;
}

static int hal3_flush(const camera3_device_t *dev)
{
    if (!dev || !dev->priv) return -EINVAL;
    struct camera_context *ctx = (struct camera_context *)dev->priv;
    if (ctx->core_handle && fn_Flush) fn_Flush(ctx->core_handle);
    return 0;
}

static void hal3_get_metadata_vendor_tag_ops(const camera3_device_t *dev,
                                              vendor_tag_query_ops_t *ops)
{
    (void)dev; (void)ops;
}

static void hal3_dump(const camera3_device_t *dev, int fd)
{
    (void)dev; (void)fd;
}

static camera3_device_ops_t g_hal3_ops = {
    .initialize                         = hal3_initialize,
    .configure_streams                  = hal3_configure_streams,
    .register_stream_buffers            = hal3_register_stream_buffers,
    .construct_default_request_settings = hal3_construct_default_request_settings,
    .process_capture_request            = hal3_process_capture_request,
    .get_metadata_vendor_tag_ops        = hal3_get_metadata_vendor_tag_ops,
    .dump                               = hal3_dump,
    .flush                              = hal3_flush,
    .reserved                           = {NULL},
};

/* -------------------------------------------------------------------------- */
/* camera_module_t                                                             */
/* -------------------------------------------------------------------------- */

static int g_detect_done;
static camera_metadata_t *g_static_info;

static int g_num_cameras;

static int hal3_get_number_of_cameras(void)
{
    if (!g_detect_done) {
        g_logf = fopen("/data/camera_hal.log", "w");
        FLOG("=== Camera HAL3 init ===\n");

        if (load_libs() != 0) {
            FLOG("FATAL: load_libs failed\n");
            g_detect_done = 1;
            g_num_cameras = 0;
            return 0;
        }

        if (fn_DeviceDetect) {
            FLOG("NvMMCameraDeviceDetect...\n");
            fn_DeviceDetect();
            FLOG("NvMMCameraDeviceDetect done\n");
        }

        g_static_info = build_static_info();
        FLOG("static_info=%p alloc_meta=%p add_meta=%p\n",
             g_static_info, fn_alloc_meta, fn_add_meta);

        /* If metadata failed, report 0 cameras to prevent crash */
        if (!g_static_info) {
            FLOG("WARNING: no metadata, reporting 0 cameras\n");
            g_num_cameras = 0;
        } else {
            g_num_cameras = 1;
        }
        g_detect_done = 1;
    }
    return g_num_cameras;
}

static int hal3_get_camera_info(int camera_id, struct camera_info *info)
{
    if (!g_detect_done) hal3_get_number_of_cameras();
    if (camera_id < 0 || camera_id >= g_num_cameras) return -EINVAL;
    if (!g_static_info) return -ENODEV;

    info->facing = CAMERA_FACING_FRONT;
    info->orientation = 270;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_0;
    info->static_camera_characteristics = g_static_info;
    FLOG("get_camera_info: id=%d OK\n", camera_id);
    return 0;
}

static int hal3_device_close(hw_device_t *device)
{
    if (!device) return -EINVAL;
    camera3_device_t *dev = (camera3_device_t *)device;
    struct camera_context *ctx = (struct camera_context *)dev->priv;

    FLOG("device_close\n");
    if (ctx) {
        if (ctx->core_handle) {
            if (fn_Flush) fn_Flush(ctx->core_handle);
            if (fn_Close) fn_Close(ctx->core_handle);
        }
        free(ctx);
    }
    dev->priv = NULL;
    free(dev);
    return 0;
}

static int hal3_device_open(const hw_module_t *module, const char *id,
                            hw_device_t **device)
{
    if (!id || !device) return -EINVAL;

    int camera_id = atoi(id);
    if (camera_id != 0) return -ENODEV;

    if (load_libs() != 0) return -ENODEV;

    camera3_device_t *dev = (camera3_device_t *)calloc(1, sizeof(*dev));
    struct camera_context *ctx = (struct camera_context *)calloc(1, sizeof(*ctx));
    if (!dev || !ctx) { free(dev); free(ctx); return -ENOMEM; }

    ctx->camera_id = camera_id;

    /* Open NvCameraCore — GUID=1 for OV5693 front */
    NvError err = fn_Open(&ctx->core_handle, 1);
    FLOG("NvCameraCore_Open(1) -> %d handle=%p\n", err, ctx->core_handle);
    if (err != NvSuccess) {
        ALOGE("NvCameraCore_Open failed: %d", err);
        free(ctx); free(dev);
        return -EIO;
    }

    fn_Callback(ctx->core_handle, ctx, nvcamera_callback);

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CAMERA_DEVICE_API_VERSION_3_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = hal3_device_close;
    dev->ops = &g_hal3_ops;
    dev->priv = ctx;

    *device = &dev->common;
    FLOG("device_open: OK\n");
    return 0;
}

static int hal3_set_callbacks(const camera_module_callbacks_t *callbacks)
{
    (void)callbacks;
    FLOG("set_callbacks: %p\n", callbacks);
    return 0;
}

static void hal3_get_vendor_tag_ops(vendor_tag_ops_t *ops)
{
    (void)ops;
}

static hw_module_methods_t g_module_methods = {
    .open = hal3_device_open,
};

extern "C" {
camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag                = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_1,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id                 = CAMERA_HARDWARE_MODULE_ID,
        .name               = "Minimal Tegra K1 Camera HAL3",
        .author             = "custom_tegra_camera",
        .methods            = &g_module_methods,
        .dso                = NULL,
        .reserved           = {0},
    },
    .get_number_of_cameras = hal3_get_number_of_cameras,
    .get_camera_info       = hal3_get_camera_info,
    .set_callbacks         = hal3_set_callbacks,
    .get_vendor_tag_ops    = hal3_get_vendor_tag_ops,
};
}
