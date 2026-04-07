/*
 * Minimal Camera HAL v1 for Tegra K1 (Xiaomi Mi Pad mocha).
 *
 * Calls into stock libnvmm_camera_v3.so via NvCameraCore API (dlopen).
 * Stock blobs handle sensor init, VI/CSI, ISP — we just wrap in HAL interface.
 */

#define LOG_TAG "CameraHAL"

#include <cutils/log.h>
#include <hardware/camera.h>
#include <hardware/hardware.h>

#include <stdio.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvcamera_core_api.h"

/* Pixel formats & gralloc usage (from system/graphics.h, hardware/gralloc.h) */
#define HAL_PIXEL_FORMAT_YCrCb_420_SP   0x11  /* NV21 */
#define GRALLOC_USAGE_SW_WRITE_OFTEN    0x00000200
#define GRALLOC_USAGE_HW_TEXTURE        0x00000100
#define GRALLOC_USAGE_HW_CAMERA_WRITE   0x00020000

/* -------------------------------------------------------------------------- */
/* NvCameraCore API — loaded at runtime via dlopen                            */
/* -------------------------------------------------------------------------- */

static void *g_nvmm_lib = NULL;
static void *g_nvgr_lib = NULL;
static pfn_nvgr_get_surfaces fn_nvgr_get_surfaces;

static pfn_NvCameraCore_Open                    fn_Open;
static pfn_NvCameraCore_Close                   fn_Close;
static pfn_NvCameraCore_CallbackFunction        fn_CallbackFunction;
static pfn_NvCameraCore_GetStaticProperties     fn_GetStaticProperties;
static pfn_NvCameraCore_GetDefaultControlProperties fn_GetDefaultControlProperties;
static pfn_NvCameraCore_SetSensorMode           fn_SetSensorMode;
static pfn_NvCameraCore_FrameCaptureRequest     fn_FrameCaptureRequest;
static pfn_NvCameraCore_Flush                   fn_Flush;
static pfn_NvMMCameraDeviceDetect               fn_DeviceDetect;

static int load_nvcamera_core(void)
{
    if (g_nvmm_lib)
        return 0;

    g_nvmm_lib = dlopen("libnvmm_camera_v3.so", RTLD_NOW);
    if (!g_nvmm_lib) {
        ALOGE("Failed to load libnvmm_camera_v3.so: %s", dlerror());
        return -ENOENT;
    }

#define LOAD_SYM(name) \
    fn_##name = (pfn_NvCameraCore_##name)dlsym(g_nvmm_lib, "NvCameraCore_" #name); \
    if (!fn_##name) { \
        ALOGE("Missing symbol: NvCameraCore_%s", #name); \
        return -ENOSYS; \
    }

    LOAD_SYM(Open);
    LOAD_SYM(Close);
    LOAD_SYM(CallbackFunction);
    LOAD_SYM(GetStaticProperties);
    LOAD_SYM(GetDefaultControlProperties);
    LOAD_SYM(SetSensorMode);
    LOAD_SYM(FrameCaptureRequest);
    LOAD_SYM(Flush);

#undef LOAD_SYM

    fn_DeviceDetect = (pfn_NvMMCameraDeviceDetect)dlsym(g_nvmm_lib,
                                                         "NvMMCameraDeviceDetect");

    /* Load nvgr for buffer conversion */
    g_nvgr_lib = dlopen("libnvgr.so", RTLD_NOW);
    if (g_nvgr_lib) {
        fn_nvgr_get_surfaces = (pfn_nvgr_get_surfaces)dlsym(g_nvgr_lib,
                                                             "nvgr_get_surfaces");
        if (!fn_nvgr_get_surfaces)
            ALOGW("nvgr_get_surfaces not found — buffer conversion unavailable");
    } else {
        ALOGW("libnvgr.so not found: %s", dlerror());
    }

    ALOGI("NvCameraCore API loaded successfully (nvgr=%s)",
          fn_nvgr_get_surfaces ? "yes" : "no");
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Per-camera device context                                                  */
/* -------------------------------------------------------------------------- */

struct camera_context {
    camera_device_t         device;
    NvCameraCoreHandle      core_handle;
    int                     camera_id;

    /* Preview state */
    preview_stream_ops_t   *preview_window;
    volatile int            preview_running;
    pthread_t               preview_thread;

    /* Callbacks from framework */
    camera_notify_callback          notify_cb;
    camera_data_callback            data_cb;
    camera_data_timestamp_callback  data_cb_timestamp;
    camera_request_memory           get_memory;
    void                           *cb_cookie;
    int32_t                         msg_type;
};

/* State shared between preview thread and NvCameraCore callback */
static volatile int g_frame_done;

/*
 * Sensor table: only OV5693 front for now (IMX179 rear has HW issue).
 * GUID = 0 means "use sensor index" — stock HAL populates GUIDs dynamically
 * via NvMMCameraDeviceDetect(). We try index-based open first.
 */
static const struct {
    NvU64 guid;
    int   facing;
    int   orientation;
} g_sensors[] = {
    { 1, CAMERA_FACING_FRONT, 270 },
};

#define NUM_CAMERAS (sizeof(g_sensors) / sizeof(g_sensors[0]))

/* -------------------------------------------------------------------------- */
/* NvCameraCore event callback                                                */
/* -------------------------------------------------------------------------- */

static NvError nvcamera_event_callback(void *pContext, NvU32 EventType,
                                       NvU32 EventInfoSize, void *pEventInfo)
{
    struct camera_context *ctx = (struct camera_context *)pContext;
    (void)EventInfoSize;

    switch (EventType) {
    case NvCameraCoreEvent_Shutter:
        ALOGV("Event: Shutter (frame %u)",
              ((NvCameraCoreShutterEventInfo *)pEventInfo)->FrameNumber);
        if (ctx->notify_cb && (ctx->msg_type & CAMERA_MSG_SHUTTER))
            ctx->notify_cb(CAMERA_MSG_SHUTTER, 0, 0, ctx->cb_cookie);
        break;

    case NvCameraCoreEvent_CompletedBuffer: {
        NvCameraCoreFrameCaptureResult *result =
            (NvCameraCoreFrameCaptureResult *)pEventInfo;
        ALOGV("Event: CompletedBuffer (frame %u, %u buffers)",
              result->FrameNumber, result->NumCompletedOutputBuffers);
        g_frame_done = 1;
        break;
    }

    case NvCameraCoreEvent_Error:
        ALOGE("Event: Error from NvCameraCore");
        if (ctx->notify_cb)
            ctx->notify_cb(CAMERA_MSG_ERROR, CAMERA_ERROR_SERVER_DIED, 0,
                           ctx->cb_cookie);
        break;

    default:
        ALOGV("Event: unknown type 0x%x", EventType);
        break;
    }

    return NvSuccess;
}

/* -------------------------------------------------------------------------- */
/* camera_device_ops_t implementation                                         */
/* -------------------------------------------------------------------------- */

static int hal_set_preview_window(struct camera_device *dev,
                                  struct preview_stream_ops *window)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ALOGI("set_preview_window: %p", window);
    ctx->preview_window = window;
    return 0;
}

static void hal_set_callbacks(struct camera_device *dev,
                              camera_notify_callback notify_cb,
                              camera_data_callback data_cb,
                              camera_data_timestamp_callback data_cb_timestamp,
                              camera_request_memory get_memory,
                              void *user)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ctx->notify_cb = notify_cb;
    ctx->data_cb = data_cb;
    ctx->data_cb_timestamp = data_cb_timestamp;
    ctx->get_memory = get_memory;
    ctx->cb_cookie = user;
}

static void hal_enable_msg_type(struct camera_device *dev, int32_t msg_type)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ctx->msg_type |= msg_type;
}

static void hal_disable_msg_type(struct camera_device *dev, int32_t msg_type)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ctx->msg_type &= ~msg_type;
}

static int hal_msg_type_enabled(struct camera_device *dev, int32_t msg_type)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    return (ctx->msg_type & msg_type) != 0;
}

/*
 * Convert gralloc buffer_handle_t to NvMMBuffer using nvgr_get_surfaces.
 * NVIDIA gralloc handles contain NvRmSurface data directly.
 */
static int gralloc_to_nvmm(buffer_handle_t *buf, NvMMBuffer *nvmm, NvU32 buf_id)
{
    if (!fn_nvgr_get_surfaces || !buf || !*buf) {
        ALOGE("gralloc_to_nvmm: nvgr not available or null buffer");
        return -1;
    }

    const NvRmSurface *surfs = NULL;
    size_t surf_count = 0;

    fn_nvgr_get_surfaces((void *)*buf, &surfs, &surf_count);
    if (!surfs || surf_count == 0) {
        ALOGE("gralloc_to_nvmm: nvgr_get_surfaces returned no surfaces");
        return -1;
    }

    if (buf_id < 3) {
        ALOGI("gralloc_to_nvmm: buf_id=%u surf_count=%zu "
              "surf[0]: %ux%u fmt=0x%x pitch=%u hMem=%p",
              buf_id, surf_count,
              surfs[0].Width, surfs[0].Height,
              surfs[0].ColorFormat, surfs[0].Pitch, surfs[0].hMem);
    }

    memset(nvmm, 0, sizeof(*nvmm));
    nvmm->StructSize = sizeof(NvMMBuffer);
    nvmm->BufferID = buf_id;
    nvmm->PayloadType = NvMMPayloadType_SurfaceArray;

    NvMMSurfaceDescriptor *desc = &nvmm->Payload.Surfaces;
    memcpy(desc->Surfaces, surfs,
           sizeof(NvRmSurface) * (surf_count > 3 ? 3 : surf_count));
    desc->SurfaceCount = surf_count;
    desc->Empty = NV_TRUE;

    return 0;
}

/* Preview thread: dequeue → convert → NvCameraCore capture → enqueue */
static void *preview_thread_func(void *arg)
{
    struct camera_context *ctx = (struct camera_context *)arg;
    preview_stream_ops_t *win = ctx->preview_window;
    int preview_w = 640;
    int preview_h = 480;
    int frame_count = 0;
    NvMMBuffer nvmm_buf;
    NvMMBuffer *out_bufs[1];

    /* Write diagnostic log to file — logcat buffer overflows with NvOsDebugPrintf spam */
    FILE *logf = fopen("/sdcard/camera_hal.log", "w");

    ALOGI("preview_thread: configuring window %dx%d NV21 (nvgr=%s)",
          preview_w, preview_h, fn_nvgr_get_surfaces ? "yes" : "no");
    if (logf) fprintf(logf, "preview_thread: start nvgr=%d\n", !!fn_nvgr_get_surfaces);

    /* Configure preview window */
    win->set_usage(win, GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_HW_TEXTURE);
    win->set_buffer_count(win, 4);
    win->set_buffers_geometry(win, preview_w, preview_h, HAL_PIXEL_FORMAT_YCrCb_420_SP);

    if (logf) fprintf(logf, "sizeof: NvMMBuffer=%zu NvMMCameraSensorMode=%zu NvRmSurface=%zu\n",
                      sizeof(NvMMBuffer), sizeof(NvMMCameraSensorMode), sizeof(NvRmSurface));

    /* Set sensor mode — OV5693 native 2592x1944, but request preview size */
    NvMMCameraSensorMode mode;
    memset(&mode, 0, sizeof(mode));
    mode.Resolution.width = preview_w;
    mode.Resolution.height = preview_h;
    mode.FrameRate = 30.0f;
    NvError err = fn_SetSensorMode(ctx->core_handle, mode);
    if (logf) { fprintf(logf, "SetSensorMode %dx%d@30 -> %d\n", preview_w, preview_h, err); fflush(logf); }

    while (ctx->preview_running) {
        buffer_handle_t *buf = NULL;
        int stride = 0;

        int ret = win->dequeue_buffer(win, &buf, &stride);
        if (ret != 0 || !buf) {
            ALOGE("preview_thread: dequeue_buffer failed: %d", ret);
            usleep(10000);
            continue;
        }

        /* Convert gralloc buffer → NvMMBuffer and submit capture request */
        if (fn_nvgr_get_surfaces) {
            int cvt = gralloc_to_nvmm(buf, &nvmm_buf, frame_count);
            if (cvt == 0) {
                out_bufs[0] = &nvmm_buf;

                NvCameraCoreFrameCaptureRequest req;
                memset(&req, 0, sizeof(req));
                req.FrameNumber = frame_count;
                req.NumOfOutputBuffers = 1;
                req.ppOutputBuffers = out_bufs;

                g_frame_done = 0;
                err = fn_FrameCaptureRequest(ctx->core_handle, &req);

                if (logf && frame_count < 5)
                    fprintf(logf, "FrameCaptureRequest frame %d -> %d\n", frame_count, err);

                if (err == NvSuccess) {
                    /* Wait for CompletedBuffer callback (max 200ms) */
                    int wait = 0;
                    while (!g_frame_done && wait < 200) {
                        usleep(1000);
                        wait++;
                    }
                    if (logf && frame_count < 5)
                        fprintf(logf, "frame %d done=%d wait=%dms\n",
                                frame_count, g_frame_done, wait);
                } else if (frame_count < 10) {
                    ALOGE("preview_thread: FrameCaptureRequest failed: %d", err);
                }
            } else if (frame_count < 3) {
                ALOGE("preview_thread: gralloc_to_nvmm failed for frame %d", frame_count);
            }
        } else if (frame_count == 0) {
            ALOGW("preview_thread: no nvgr — skipping capture");
        }

        win->set_timestamp(win, frame_count * 33333333LL);
        win->enqueue_buffer(win, buf);

        frame_count++;
        if (frame_count % 10000 == 0)
            ALOGI("preview_thread: %d frames", frame_count);
    }

    ALOGI("preview_thread: stopped after %d frames", frame_count);
    if (logf) { fprintf(logf, "stopped after %d frames\n", frame_count); fclose(logf); }
    return NULL;
}

static int hal_start_preview(struct camera_device *dev)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ALOGI("start_preview (camera %d)", ctx->camera_id);

    if (!ctx->core_handle) {
        ALOGE("start_preview: camera not opened");
        return -EINVAL;
    }

    if (!ctx->preview_window) {
        ALOGE("start_preview: no preview window set");
        return -EINVAL;
    }

    if (ctx->preview_running) {
        ALOGW("start_preview: already running");
        return 0;
    }

    ctx->preview_running = 1;

    int ret = pthread_create(&ctx->preview_thread, NULL, preview_thread_func, ctx);
    if (ret != 0) {
        ALOGE("start_preview: pthread_create failed: %d", ret);
        ctx->preview_running = 0;
        return -ret;
    }

    ALOGI("start_preview: preview thread started");
    return 0;
}

static void hal_stop_preview(struct camera_device *dev)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ALOGI("stop_preview (camera %d)", ctx->camera_id);

    if (ctx->preview_running) {
        ctx->preview_running = 0;
        pthread_join(ctx->preview_thread, NULL);
    }

    if (ctx->core_handle)
        fn_Flush(ctx->core_handle);
}

static int hal_preview_enabled(struct camera_device *dev)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    return ctx->preview_running;
}

static int hal_store_meta_data_in_buffers(struct camera_device *dev, int enable)
{
    (void)dev; (void)enable;
    return enable ? -EINVAL : 0;
}

static int hal_start_recording(struct camera_device *dev)
{
    (void)dev;
    ALOGW("start_recording: not implemented");
    return -EINVAL;
}

static void hal_stop_recording(struct camera_device *dev)
{
    (void)dev;
}

static int hal_recording_enabled(struct camera_device *dev)
{
    (void)dev;
    return 0;
}

static void hal_release_recording_frame(struct camera_device *dev,
                                        const void *opaque)
{
    (void)dev; (void)opaque;
}

static int hal_auto_focus(struct camera_device *dev)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ALOGV("auto_focus");
    /* For now, immediately report focus complete */
    if (ctx->notify_cb && (ctx->msg_type & CAMERA_MSG_FOCUS))
        ctx->notify_cb(CAMERA_MSG_FOCUS, 1, 0, ctx->cb_cookie);
    return 0;
}

static int hal_cancel_auto_focus(struct camera_device *dev)
{
    (void)dev;
    return 0;
}

static int hal_take_picture(struct camera_device *dev)
{
    (void)dev;
    ALOGW("take_picture: not implemented");
    return -EINVAL;
}

static int hal_cancel_picture(struct camera_device *dev)
{
    (void)dev;
    return 0;
}

static int hal_set_parameters(struct camera_device *dev, const char *params)
{
    (void)dev;
    ALOGV("set_parameters: %s", params ? params : "(null)");
    /* TODO: parse resolution, format, fps from params string */
    return 0;
}

static char *hal_get_parameters(struct camera_device *dev)
{
    struct camera_context *ctx = (struct camera_context *)dev;

    /*
     * Return minimal parameter string.
     * Camera service parses these to know supported resolutions, formats, etc.
     * TODO: query NvCameraCore_GetStaticProperties for real values.
     */
    const char *params;
    /* Camera 0 = OV5693 front: 5MP 2592x1944, preview 1920x1080 */
    (void)ctx;
    params =
        "preview-size=1920x1080"
        ";preview-size-values=1920x1080,1280x720,640x480"
        ";picture-size=2592x1944"
        ";picture-size-values=2592x1944,1920x1080"
        ";preview-format=yuv420sp"
        ";preview-format-values=yuv420sp,yuv420p"
        ";preview-fps-range=15000,30000"
        ";preview-frame-rate=30"
        ";picture-format=jpeg"
        ";focus-mode=fixed"
        ";focus-mode-values=fixed"
        ";jpeg-quality=85";

    /* Camera service calls put_parameters() to free this */
    char *ret = strdup(params);
    return ret;
}

static void hal_put_parameters(struct camera_device *dev, char *params)
{
    (void)dev;
    free(params);
}

static int hal_send_command(struct camera_device *dev,
                            int32_t cmd, int32_t arg1, int32_t arg2)
{
    (void)dev; (void)cmd; (void)arg1; (void)arg2;
    return 0;
}

static void hal_release(struct camera_device *dev)
{
    struct camera_context *ctx = (struct camera_context *)dev;
    ALOGI("release (camera %d)", ctx->camera_id);

    if (ctx->preview_running)
        hal_stop_preview(dev);

    if (ctx->core_handle) {
        fn_Close(ctx->core_handle);
        ctx->core_handle = NULL;
    }
}

static int hal_dump(struct camera_device *dev, int fd)
{
    (void)dev; (void)fd;
    return 0;
}

static camera_device_ops_t g_camera_ops = {
    .set_preview_window         = hal_set_preview_window,
    .set_callbacks              = hal_set_callbacks,
    .enable_msg_type            = hal_enable_msg_type,
    .disable_msg_type           = hal_disable_msg_type,
    .msg_type_enabled           = hal_msg_type_enabled,
    .start_preview              = hal_start_preview,
    .stop_preview               = hal_stop_preview,
    .preview_enabled            = hal_preview_enabled,
    .store_meta_data_in_buffers = hal_store_meta_data_in_buffers,
    .start_recording            = hal_start_recording,
    .stop_recording             = hal_stop_recording,
    .recording_enabled          = hal_recording_enabled,
    .release_recording_frame    = hal_release_recording_frame,
    .auto_focus                 = hal_auto_focus,
    .cancel_auto_focus          = hal_cancel_auto_focus,
    .take_picture               = hal_take_picture,
    .cancel_picture             = hal_cancel_picture,
    .set_parameters             = hal_set_parameters,
    .get_parameters             = hal_get_parameters,
    .put_parameters             = hal_put_parameters,
    .send_command               = hal_send_command,
    .release                    = hal_release,
    .dump                       = hal_dump,
};

/* -------------------------------------------------------------------------- */
/* camera_module_t implementation                                             */
/* -------------------------------------------------------------------------- */

static int g_device_detect_done = 0;

static int hal_get_number_of_cameras(void)
{
    /* Run device detection once — initializes PCL, powers on sensors,
     * writes layout to kernel. Required before NvCameraCore_Open. */
    if (!g_device_detect_done) {
        int ret = load_nvcamera_core();
        if (ret == 0 && fn_DeviceDetect) {
            ALOGI("Running NvMMCameraDeviceDetect...");
            NvBool detected = fn_DeviceDetect();
            ALOGI("NvMMCameraDeviceDetect: %s", detected ? "OK" : "FAILED");
        }
        g_device_detect_done = 1;
    }

    ALOGI("get_number_of_cameras: %d", (int)NUM_CAMERAS);
    return NUM_CAMERAS;
}

static int hal_get_camera_info(int camera_id, struct camera_info *info)
{
    if (camera_id < 0 || camera_id >= (int)NUM_CAMERAS)
        return -EINVAL;

    info->facing = g_sensors[camera_id].facing;
    info->orientation = g_sensors[camera_id].orientation;
    return 0;
}

static int hal_device_close(hw_device_t *device)
{
    struct camera_context *ctx = (struct camera_context *)device;
    ALOGI("device_close (camera %d)", ctx->camera_id);

    hal_release(&ctx->device);
    free(ctx);
    return 0;
}

static int hal_device_open(const hw_module_t *module, const char *id,
                           hw_device_t **device)
{
    int camera_id;
    int ret;

    if (!id || !device)
        return -EINVAL;

    camera_id = atoi(id);
    if (camera_id < 0 || camera_id >= (int)NUM_CAMERAS) {
        ALOGE("Invalid camera id: %d", camera_id);
        return -EINVAL;
    }

    ret = load_nvcamera_core();
    if (ret) {
        ALOGE("Failed to load NvCameraCore: %d", ret);
        return ret;
    }

    struct camera_context *ctx =
        (struct camera_context *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;

    ctx->camera_id = camera_id;

    /* Open NvCameraCore for this sensor */
    NvError err = fn_Open(&ctx->core_handle, g_sensors[camera_id].guid);
    if (err != NvSuccess) {
        ALOGE("NvCameraCore_Open failed: %d (camera %d, guid 0x%llx)",
              err, camera_id, (unsigned long long)g_sensors[camera_id].guid);
        free(ctx);
        return -EIO;
    }

    /* Register event callback */
    err = fn_CallbackFunction(ctx->core_handle, ctx, nvcamera_event_callback);
    if (err != NvSuccess) {
        ALOGE("NvCameraCore_CallbackFunction failed: %d", err);
        fn_Close(ctx->core_handle);
        free(ctx);
        return -EIO;
    }

    ALOGI("Camera %d opened (handle=%p)", camera_id, ctx->core_handle);

    /* Wire up hw_device_t */
    ctx->device.common.tag = HARDWARE_DEVICE_TAG;
    ctx->device.common.version = CAMERA_DEVICE_API_VERSION_1_0;
    ctx->device.common.module = (hw_module_t *)module;
    ctx->device.common.close = hal_device_close;
    ctx->device.ops = &g_camera_ops;

    *device = &ctx->device.common;
    return 0;
}

static hw_module_methods_t g_module_methods = {
    .open = hal_device_open,
};

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag            = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id             = CAMERA_HARDWARE_MODULE_ID,
        .name           = "Minimal Tegra K1 Camera HAL",
        .author         = "custom_tegra_camera",
        .methods        = &g_module_methods,
        .dso            = NULL,
        .reserved       = {0},
    },
    .get_number_of_cameras = hal_get_number_of_cameras,
    .get_camera_info       = hal_get_camera_info,
};
