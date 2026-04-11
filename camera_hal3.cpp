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
#include <hardware/gralloc.h>
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
#include "vendor_ops.h"

/* Pixel formats & gralloc usage */
#define HAL_PIXEL_FORMAT_YCrCb_420_SP       0x11
#define HAL_PIXEL_FORMAT_YV12               0x32315659
#define HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED 0x22
#define GRALLOC_USAGE_HW_CAMERA_WRITE       0x00020000
#define GRALLOC_USAGE_HW_CAMERA_READ        0x00010000
#define GRALLOC_USAGE_SW_READ_OFTEN         0x00000003

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

/* NvRm functions for buffer allocation + direct access (loaded from libnvrm.so) */
typedef int (*pfn_NvRmOpen)(void **phRm, NvU32 devId);
typedef void (*pfn_NvRmClose)(void *hRm);
typedef int (*pfn_NvRmMemMap)(void *hMem, NvU32 offset, NvU32 size, NvU32 flags, void **pVirtAddr);
typedef void (*pfn_NvRmMemUnmap)(void *hMem, void *pVirtAddr, NvU32 size);
typedef int (*pfn_NvRmMemCacheSyncForCpu)(void *hMem, void *pVirtAddr, NvU32 size);
typedef void (*pfn_NvRmMemRead)(void *hMem, NvU32 offset, void *dst, NvU32 size);
typedef NvU32 (*pfn_NvRmSurfaceComputeSize)(const NvRmSurface *surf);
typedef void (*pfn_NvRmMultiplanarSurfaceSetup)(void *desc, NvU32 numSurfs,
    NvU32 width, NvU32 height, NvU32 layout, void *colorFormats, void *attrs);
typedef int (*pfn_NvRmMemHandleAllocAttr)(void *hRm, void *attrs, void **phMem);
typedef NvU32 (*pfn_NvRmSurfaceComputeAlignment)(void *hRm, const NvRmSurface *surf);
typedef void (*pfn_NvRmMemHandleFree)(void *hMem);

static pfn_NvRmOpen fn_NvRmOpen;
static pfn_NvRmClose fn_NvRmClose;
static pfn_NvRmMemMap fn_NvRmMemMap;
static pfn_NvRmMemUnmap fn_NvRmMemUnmap;
static pfn_NvRmMemCacheSyncForCpu fn_NvRmMemCacheSyncForCpu;
static pfn_NvRmMemRead fn_NvRmMemRead;
static pfn_NvRmSurfaceComputeSize fn_NvRmSurfaceComputeSize;
static pfn_NvRmMultiplanarSurfaceSetup fn_NvRmMultiplanarSurfaceSetup;
static pfn_NvRmMemHandleAllocAttr fn_NvRmMemHandleAllocAttr;
static pfn_NvRmSurfaceComputeAlignment fn_NvRmSurfaceComputeAlignment;
static pfn_NvRmMemHandleFree fn_NvRmMemHandleFree;

/* gralloc module for lock/unlock */
static const gralloc_module_t *g_gralloc;

/* camera_metadata — use dlsym at runtime (not linked directly due to SDK_VERSION=19) */
typedef camera_metadata_t *(*pfn_allocate_camera_metadata)(size_t, size_t);
typedef int (*pfn_add_camera_metadata_entry)(camera_metadata_t*, uint32_t, const void*, size_t);
typedef void (*pfn_sort_camera_metadata)(camera_metadata_t*);

static pfn_allocate_camera_metadata fn_alloc_meta;
static pfn_add_camera_metadata_entry fn_add_meta;
static pfn_sort_camera_metadata fn_sort_meta;

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
        LSYM(fn_sort_meta, meta_lib, "sort_camera_metadata");
    }

#undef LSYM

    if (!fn_Open || !fn_Close || !fn_FrameCaptureRequest) return -1;

    /* Load NvRm functions for buffer allocation + direct access */
    void *nvrm_lib = dlopen("libnvrm.so", RTLD_NOW);
    if (nvrm_lib) {
        fn_NvRmOpen = (pfn_NvRmOpen)dlsym(nvrm_lib, "NvRmOpen");
        fn_NvRmClose = (pfn_NvRmClose)dlsym(nvrm_lib, "NvRmClose");
        fn_NvRmMemMap = (pfn_NvRmMemMap)dlsym(nvrm_lib, "NvRmMemMap");
        fn_NvRmMemUnmap = (pfn_NvRmMemUnmap)dlsym(nvrm_lib, "NvRmMemUnmap");
        fn_NvRmMemCacheSyncForCpu = (pfn_NvRmMemCacheSyncForCpu)dlsym(nvrm_lib, "NvRmMemCacheSyncForCpu");
        fn_NvRmMemRead = (pfn_NvRmMemRead)dlsym(nvrm_lib, "NvRmMemRead");
        fn_NvRmSurfaceComputeSize = (pfn_NvRmSurfaceComputeSize)dlsym(nvrm_lib, "NvRmSurfaceComputeSize");
        fn_NvRmMultiplanarSurfaceSetup = (pfn_NvRmMultiplanarSurfaceSetup)dlsym(nvrm_lib, "NvRmMultiplanarSurfaceSetup");
        fn_NvRmMemHandleAllocAttr = (pfn_NvRmMemHandleAllocAttr)dlsym(nvrm_lib, "NvRmMemHandleAllocAttr");
        fn_NvRmSurfaceComputeAlignment = (pfn_NvRmSurfaceComputeAlignment)dlsym(nvrm_lib, "NvRmSurfaceComputeAlignment");
        fn_NvRmMemHandleFree = (pfn_NvRmMemHandleFree)dlsym(nvrm_lib, "NvRmMemHandleFree");
        FLOG("NvRm loaded: Map=%d Setup=%d Alloc=%d\n",
             !!fn_NvRmMemMap, !!fn_NvRmMultiplanarSurfaceSetup, !!fn_NvRmMemHandleAllocAttr);
    }

    /* Load gralloc module for lock/unlock */
    hw_module_t const *gralloc_hw = NULL;
    if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &gralloc_hw) == 0)
        g_gralloc = (const gralloc_module_t *)gralloc_hw;

    ALOGI("NvCameraCore loaded (nvgr=%d meta=%d gralloc=%d)",
          !!fn_nvgr_get_surfaces, !!fn_alloc_meta, !!g_gralloc);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Minimal camera_metadata builders                                            */
/* -------------------------------------------------------------------------- */

/*
 * Build static camera characteristics matching stock MIUI camera.tegra.so dump.
 * Camera 1 = OV5693 front (facing=1, orientation=270).
 * 53 entries, ~880 bytes extra data.
 */
static camera_metadata_t *build_static_info(int camera_id)
{
    if (!fn_alloc_meta || !fn_add_meta) return NULL;

    camera_metadata_t *m = fn_alloc_meta(64, 2048);
    if (!m) return NULL;

    /* --- android.control --- */
    uint8_t antibanding[] = {0, 1, 2, 3}; /* OFF, 50HZ, 60HZ, AUTO */
    uint8_t ae_modes[] = {0, 1};           /* OFF, ON */
    int32_t ae_fps[] = {15, 30, 30, 30};   /* [min,max] x 2 ranges */
    int32_t ae_comp_range[] = {-4, 4};
    int32_t ae_comp_step[] = {1, 2};       /* rational: 1/2 = 0.5 */
    uint8_t af_modes[] = {0};              /* OFF (front camera, no AF) */
    uint8_t effects[] = {0, 1, 2, 3, 4, 5, 8}; /* OFF..AQUA (7 entries like stock) */
    uint8_t scene_modes[16]; memset(scene_modes, 0, 16); /* 16 scene modes */
    uint8_t vstab[] = {0, 1};              /* OFF, ON */
    uint8_t awb_modes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8}; /* OFF..SHADE (9) */
    int32_t max_regions = 0;
    uint8_t scene_overrides[48]; memset(scene_overrides, 0, 48); /* 16 modes x 3 */
    int32_t high_fps[] = {0, 0, 0};        /* no high-fps modes */

    fn_add_meta(m, MIUI_CONTROL_AE_AVAIL_ANTIBANDING, antibanding, 4);
    fn_add_meta(m, MIUI_CONTROL_AE_AVAIL_MODES, ae_modes, 2);
    fn_add_meta(m, MIUI_CONTROL_AE_AVAIL_FPS_RANGES, ae_fps, 4);
    fn_add_meta(m, MIUI_CONTROL_AE_COMP_RANGE, ae_comp_range, 2);
    fn_add_meta(m, MIUI_CONTROL_AE_COMP_STEP, ae_comp_step, 1); /* rational[1] */
    fn_add_meta(m, MIUI_CONTROL_AF_AVAIL_MODES, af_modes, 1);
    fn_add_meta(m, MIUI_CONTROL_AVAIL_EFFECTS, effects, 7);
    fn_add_meta(m, MIUI_CONTROL_AVAIL_SCENE_MODES, scene_modes, 16);
    fn_add_meta(m, MIUI_CONTROL_AVAIL_VSTAB_MODES, vstab, 2);
    fn_add_meta(m, MIUI_CONTROL_AWB_AVAIL_MODES, awb_modes, 9);
    fn_add_meta(m, MIUI_CONTROL_MAX_REGIONS, &max_regions, 1);
    fn_add_meta(m, MIUI_CONTROL_SCENE_MODE_OVERRIDES, scene_overrides, 48);
    fn_add_meta(m, MIUI_CONTROL_HIGH_FPS_RECORDING, high_fps, 3);

    /* --- android.flash.info --- */
    uint8_t flash_avail = 0;
    int64_t flash_charge = 0;
    fn_add_meta(m, MIUI_FLASH_INFO_AVAILABLE, &flash_avail, 1);
    fn_add_meta(m, MIUI_FLASH_INFO_CHARGE_DUR, &flash_charge, 1);

    /* --- android.jpeg --- */
    int32_t thumb_sizes[] = {0, 0, 160, 120, 320, 240};
    int32_t jpeg_max = 2592 * 1944 * 3 / 2 + 65536;
    fn_add_meta(m, MIUI_JPEG_AVAIL_THUMB_SIZES, thumb_sizes, 6);
    fn_add_meta(m, MIUI_JPEG_MAX_SIZE, &jpeg_max, 1);

    /* --- android.lens + lens.info --- */
    uint8_t facing = (camera_id == 0) ? 0 : 1; /* BACK=0, FRONT=1 */
    float lens_pos[] = {0.0f, 0.0f, 0.0f};
    float apertures[] = {2.0f};
    float focal_lengths[] = {3.5f};
    uint8_t ois_modes[] = {0}; /* OFF */
    float hyperfocal = 0.0f;
    float min_focus = 0.0f;

    fn_add_meta(m, MIUI_LENS_FACING, &facing, 1);
    fn_add_meta(m, MIUI_LENS_POSITION, lens_pos, 3);
    fn_add_meta(m, MIUI_LENS_INFO_AVAIL_APERTURES, apertures, 1);
    fn_add_meta(m, MIUI_LENS_INFO_AVAIL_FILTER_DENS, NULL, 0); /* float[0] */
    fn_add_meta(m, MIUI_LENS_INFO_AVAIL_FOCAL_LENGTHS, focal_lengths, 1);
    fn_add_meta(m, MIUI_LENS_INFO_AVAIL_OIS, ois_modes, 1);
    fn_add_meta(m, MIUI_LENS_INFO_HYPERFOCAL_DIST, &hyperfocal, 1);
    fn_add_meta(m, MIUI_LENS_INFO_MIN_FOCUS_DISTANCE, &min_focus, 1);

    /* --- android.scaler --- */
    int32_t formats[] = {0x20, 0x22, 0x11, 0x100, 0x23}; /* 5 formats like stock */
    float max_zoom = 1.0f;
    /* OV5693 processed sizes (matching stock dump) */
    int32_t proc_sizes[] = {
        2592, 1944, 2048, 1536, 1920, 1080, 1600, 1200,
        1280, 960,  1280, 720,  1024, 768,  960,  720,
        800,  600,  720,  480,  640,  480,  352,  288,
        320,  240,  176,  144,  160,  120,  2592, 1944,
        2048, 1536, 1920, 1080, 1600, 1200, 1280, 960,
        1280, 720,
    };
    int32_t n_proc = sizeof(proc_sizes) / sizeof(proc_sizes[0]);
    int64_t proc_dur[21]; for (int i = 0; i < 21; i++) proc_dur[i] = 33333333LL;
    int32_t jpeg_sizes[] = {
        2592, 1944, 2048, 1536, 1920, 1080, 1600, 1200,
        1280, 960,  1280, 720,  1024, 768,  640,  480,
        320,  240,
    };
    int32_t n_jpeg = sizeof(jpeg_sizes) / sizeof(jpeg_sizes[0]);
    int64_t jpeg_dur[9]; for (int i = 0; i < 9; i++) jpeg_dur[i] = 33333333LL;
    int32_t raw_sizes[] = {2592, 1944, 2592, 1944, 2592, 1944, 2592, 1944};
    int64_t raw_dur[] = {33333333LL, 33333333LL, 33333333LL, 33333333LL};

    fn_add_meta(m, MIUI_SCALER_AVAIL_FORMATS, formats, 5);
    fn_add_meta(m, MIUI_SCALER_AVAIL_MAX_DIGITAL_ZOOM, &max_zoom, 1);
    fn_add_meta(m, MIUI_SCALER_AVAIL_RAW_SIZES, raw_sizes, 8);
    fn_add_meta(m, MIUI_SCALER_AVAIL_RAW_MIN_DUR, raw_dur, 4);
    fn_add_meta(m, MIUI_SCALER_AVAIL_PROC_SIZES, proc_sizes, n_proc);
    fn_add_meta(m, MIUI_SCALER_AVAIL_PROC_MIN_DUR, proc_dur, n_proc / 2);
    fn_add_meta(m, MIUI_SCALER_AVAIL_JPEG_SIZES, jpeg_sizes, n_jpeg);
    fn_add_meta(m, MIUI_SCALER_AVAIL_JPEG_MIN_DUR, jpeg_dur, n_jpeg / 2);

    /* --- android.sensor + sensor.info --- */
    int32_t active_array[] = {0, 0, 2592, 1944};
    int32_t sensitivity_range[] = {100, 1600};
    uint8_t color_filter = 0; /* RGGB */
    int64_t exposure_range[] = {1000000LL, 300000000LL};
    int64_t max_frame_dur = 300000000LL;
    float physical_size[] = {3.67f, 2.74f};
    int32_t pixel_array[] = {2592, 1944};
    int32_t white_level = 4095;
    int32_t black_level[] = {64, 64, 64, 64};
    int32_t orientation = (camera_id == 0) ? 90 : 270;
    int32_t max_analog_sens = 800;

    fn_add_meta(m, MIUI_SENSOR_INFO_ACTIVE_ARRAY, active_array, 4);
    fn_add_meta(m, MIUI_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity_range, 2);
    fn_add_meta(m, MIUI_SENSOR_INFO_COLOR_FILTER, &color_filter, 1);
    fn_add_meta(m, MIUI_SENSOR_INFO_EXPOSURE_RANGE, exposure_range, 2);
    fn_add_meta(m, MIUI_SENSOR_INFO_MAX_FRAME_DUR, &max_frame_dur, 1);
    fn_add_meta(m, MIUI_SENSOR_INFO_PHYSICAL_SIZE, physical_size, 2);
    fn_add_meta(m, MIUI_SENSOR_INFO_PIXEL_ARRAY, pixel_array, 2);
    fn_add_meta(m, MIUI_SENSOR_INFO_WHITE_LEVEL, &white_level, 1);
    fn_add_meta(m, MIUI_SENSOR_BLACK_LEVEL_PATTERN, black_level, 4);
    fn_add_meta(m, MIUI_SENSOR_ORIENTATION, &orientation, 1);
    fn_add_meta(m, MIUI_SENSOR_MAX_ANALOG_SENSITIVITY, &max_analog_sens, 1);

    /* --- android.statistics.info --- */
    uint8_t face_detect[] = {0, 1}; /* OFF, SIMPLE */
    int32_t hist_buckets = 64;
    int32_t max_face = 0;
    int32_t max_hist = 1000;
    int32_t max_sharp = 1000;
    int32_t sharp_map[] = {64, 64};
    fn_add_meta(m, MIUI_STATS_INFO_AVAIL_FACE_DETECT, face_detect, 2);
    fn_add_meta(m, MIUI_STATS_INFO_HISTOGRAM_BUCKETS, &hist_buckets, 1);
    fn_add_meta(m, MIUI_STATS_INFO_MAX_FACE_COUNT, &max_face, 1);
    fn_add_meta(m, MIUI_STATS_INFO_MAX_HISTOGRAM_COUNT, &max_hist, 1);
    fn_add_meta(m, MIUI_STATS_INFO_MAX_SHARPNESS_VAL, &max_sharp, 1);
    fn_add_meta(m, MIUI_STATS_INFO_SHARPNESS_MAP_SIZE, sharp_map, 2);

    /* --- android.tonemap --- */
    int32_t tonemap_points = 128;
    fn_add_meta(m, MIUI_TONEMAP_MAX_CURVE_POINTS, &tonemap_points, 1);

    /* --- android.info --- */
    uint8_t hw_level = 0; /* LIMITED (not FULL — stock uses 0) */
    fn_add_meta(m, MIUI_INFO_HW_LEVEL, &hw_level, 1);

    /* --- android.quirks --- */
    uint8_t partial_result = 1;
    fn_add_meta(m, MIUI_QUIRKS_USE_PARTIAL_RESULT, &partial_result, 1);

    /* Vendor-specific stubs (MIUI CameraService extras) */
    vendor_ops_get()->add_static_metadata(m, fn_add_meta);

    /* Sort for CameraMetadata::find() binary search */
    if (fn_sort_meta) fn_sort_meta(m);

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

    /* Vendor-specific request stubs */
    vendor_ops_get()->add_request_metadata(m, fn_add_meta);

    if (fn_sort_meta) fn_sort_meta(m);
    return m;
}

static camera_metadata_t *build_result_meta(int64_t timestamp)
{
    if (!fn_alloc_meta || !fn_add_meta) return NULL;

    camera_metadata_t *m = fn_alloc_meta(5, 64);
    if (!m) return NULL;

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

    /* Default control properties from NvCameraCore */
    uint8_t                      default_ctrl_props[NVCAM_CONTROLS_SIZE];

    /* Sync frame tracking */
    volatile int                 frame_done;
    volatile int64_t             shutter_timestamp;
};

/* -------------------------------------------------------------------------- */
/* NvCameraCore event callback                                                 */
/* -------------------------------------------------------------------------- */

static NvError nvcamera_callback(void *ctx_ptr, NvU32 event_type,
                                  NvU32 info_size, void *info)
{
    struct camera_context *ctx = (struct camera_context *)ctx_ptr;
    (void)info_size;

    FLOG("cb: ev=%u sz=%u\n", event_type, info_size);

    if (event_type == NvCameraCoreEvent_Shutter) {
        NvCameraCoreShutterEventInfo *shutter = (NvCameraCoreShutterEventInfo *)info;
        ctx->shutter_timestamp = (int64_t)shutter->Timestamp;
        FLOG("Shutter: frame %u ts=%llu\n", shutter->FrameNumber,
             (unsigned long long)shutter->Timestamp);
        if (ctx->callback_ops) {
            camera3_notify_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = CAMERA3_MSG_SHUTTER;
            msg.message.shutter.frame_number = shutter->FrameNumber;
            msg.message.shutter.timestamp = shutter->Timestamp;
            ctx->callback_ops->notify(ctx->callback_ops, &msg);
        }
    } else if (event_type == NvCameraCoreEvent_CompletedBuffer) {
        NvCameraCoreFrameCaptureResult *res = (NvCameraCoreFrameCaptureResult *)info;
        FLOG("CompletedBuffer: frame %u, %u bufs pp=%p\n",
             res->FrameNumber, res->NumCompletedOutputBuffers,
             res->ppOutputBuffers);
        if (res->ppOutputBuffers && res->NumCompletedOutputBuffers > 0) {
            NvMMBuffer *outbuf = res->ppOutputBuffers[0];
            NvRmSurface *s0 = &outbuf->Payload.Surfaces.Surfaces[0];
            FLOG("  outbuf=%p id=%u pt=%u ss=%u empty=%u sc=%d\n",
                 outbuf, outbuf->BufferID, outbuf->PayloadType,
                 outbuf->StructSize, outbuf->Payload.Surfaces.Empty,
                 outbuf->Payload.Surfaces.SurfaceCount);
            FLOG("  s0: %ux%u fmt=0x%x layout=%u pitch=%u hMem=%p off=%u pBase=%p\n",
                 s0->Width, s0->Height, s0->ColorFormat, s0->Layout,
                 s0->Pitch, s0->hMem, s0->Offset, s0->pBase);
            if (outbuf->Payload.Surfaces.SurfaceCount > 1) {
                NvRmSurface *s1 = &outbuf->Payload.Surfaces.Surfaces[1];
                FLOG("  s1: %ux%u fmt=0x%x layout=%u pitch=%u hMem=%p off=%u pBase=%p\n",
                     s1->Width, s1->Height, s1->ColorFormat, s1->Layout,
                     s1->Pitch, s1->hMem, s1->Offset, s1->pBase);
            }
        }
        ctx->frame_done = 1;
    }

    return NvSuccess;
}

/* -------------------------------------------------------------------------- */
/* Buffer allocation: own pitchlinear NvMMBuffer (like stock AllocateNvMMSurface) */
/* -------------------------------------------------------------------------- */

static void *g_rm_handle; /* NvRm device handle */

static int alloc_nvmm_surface(NvMMBuffer *nvmm, NvU32 buf_id, NvU32 w, NvU32 h)
{
    if (!fn_NvRmMultiplanarSurfaceSetup || !fn_NvRmMemHandleAllocAttr ||
        !fn_NvRmSurfaceComputeSize || !fn_NvRmSurfaceComputeAlignment)
        return -1;

    /* Open NvRm if needed */
    if (!g_rm_handle && fn_NvRmOpen) {
        fn_NvRmOpen(&g_rm_handle, 0);
        FLOG("NvRmOpen -> %p\n", g_rm_handle);
    }
    if (!g_rm_handle) return -1;

    memset(nvmm, 0, sizeof(NvMMBuffer));
    nvmm->StructSize = sizeof(NvMMBuffer);
    nvmm->PayloadType = NvMMPayloadType_SurfaceArray;
    nvmm->BufferID = buf_id;

    NvMMSurfaceDescriptor *desc = &nvmm->Payload.Surfaces;

    /* Set up YV12 3-plane pitchlinear (stock HAL: "allocating YV12 for Zoom Stream") */
    NvRmSurface *s0 = &desc->Surfaces[0];
    NvRmSurface *s1 = &desc->Surfaces[1];
    NvRmSurface *s2 = &desc->Surfaces[2];

    NvU32 pitch = (w + 63) & ~63; /* 64-byte aligned pitch */

    /* Y plane */
    s0->Width = w;
    s0->Height = h;
    s0->ColorFormat = 0x08592004; /* NvColorFormat_Y8 */
    s0->Layout = NvRmSurfaceLayout_Pitch;
    s0->Pitch = pitch;

    /* U plane */
    s1->Width = w / 2;
    s1->Height = h / 2;
    s1->ColorFormat = 0x08590404; /* NvColorFormat_U8 (computed) */
    s1->Layout = NvRmSurfaceLayout_Pitch;
    s1->Pitch = pitch / 2;

    /* V plane */
    s2->Width = w / 2;
    s2->Height = h / 2;
    s2->ColorFormat = 0x08582404; /* NvColorFormat_V8 (computed) */
    s2->Layout = NvRmSurfaceLayout_Pitch;
    s2->Pitch = pitch / 2;

    desc->SurfaceCount = 3;
    desc->Empty = NV_TRUE;

    /* Allocate memory for each surface via NvRm */
    /* NvRmHeap enum: External=1, Carveout=2, IRAM=3, GART=4 */
    NvU32 heaps[] = { 2 /* Carveout */ };
    for (int i = 0; i < 3; i++) {
        NvRmSurface *s = &desc->Surfaces[i];
        NvU32 size = s->Pitch * s->Height;
        /* NvRmMemHandleAttr struct layout from nvrm_memmgr.h */
        struct {
            const NvU32 *Heaps;
            NvU32 NumHeaps;
            NvU32 Alignment;
            NvU32 Coherency; /* WriteBack=2 */
            NvU32 Size;
            NvU32 Tags;
            NvU32 Kind;
            NvU32 CompTags;
        } attr;
        memset(&attr, 0, sizeof(attr));
        attr.Heaps = heaps;
        attr.NumHeaps = 1;
        attr.Alignment = 64;
        attr.Coherency = 2; /* WriteBack */
        attr.Size = size;
        attr.Kind = s->Kind;
        int err = fn_NvRmMemHandleAllocAttr(g_rm_handle, &attr, &s->hMem);
        if (err != 0) {
            FLOG("NvRmMemHandleAllocAttr surf %d size=%u failed: %d\n", i, size, err);
            return -1;
        }
    }

    FLOG("alloc_nvmm: id=%u %ux%u YV12 pitch=%u hMem=%p/%p/%p\n",
         buf_id, w, h, pitch,
         desc->Surfaces[0].hMem, desc->Surfaces[1].hMem, desc->Surfaces[2].hMem);

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

        /* NV21 (2-plane YCrCb 4:2:0). YV12 crashes in NvViCsi.
         * Buffers are blocklinear — checking if ISP writes to nvmap directly. */
        if (s->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
            s->format = HAL_PIXEL_FORMAT_YCrCb_420_SP;
        s->usage = GRALLOC_USAGE_HW_CAMERA_WRITE | GRALLOC_USAGE_SW_READ_OFTEN;
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

        /* If exact mode not supported, try full sensor resolution */
        if (err != NvSuccess) {
            mode.Resolution.width = 2592;
            mode.Resolution.height = 1944;
            err = fn_SetSensorMode(ctx->core_handle, mode);
            FLOG("SetSensorMode fallback 2592x1944 -> %d\n", err);
        }
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

    /* Allocate own pitchlinear NV12 buffers for NvCameraCore output.
     * Stock HAL does this too — NvCameraCore doesn't write to gralloc blocklinear. */
    int n = buf_set->num_buffers > MAX_BUFFERS ? MAX_BUFFERS : buf_set->num_buffers;
    NvU32 w = buf_set->stream ? buf_set->stream->width : 2048;
    NvU32 h = buf_set->stream ? buf_set->stream->height : 1536;
    for (int i = 0; i < n; i++) {
        ctx->stream.anb_handles[i] = buf_set->buffers[i];
        alloc_nvmm_surface(&ctx->stream.nvmm_bufs[i], i, w, h);
    }
    ctx->stream.num_bufs = n;

    return 0;
}

static const camera_metadata_t *hal3_construct_default_request_settings(
        const camera3_device_t *dev, int type)
{
    static camera_metadata_t *s_default = NULL;
    (void)dev; (void)type;

    if (!s_default) {
        s_default = build_default_request();
        FLOG("construct_default_request_settings: type=%d result=%p\n", type, s_default);
    }
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

    /* Not pre-allocated — allocate on the fly */
    if (!nvmm && ctx->stream.num_bufs < MAX_BUFFERS) {
        int idx = ctx->stream.num_bufs;
        NvU32 w = ctx->stream.stream ? ctx->stream.stream->width : 2048;
        NvU32 h = ctx->stream.stream ? ctx->stream.stream->height : 1536;
        ctx->stream.anb_handles[idx] = out_copy.buffer;
        if (alloc_nvmm_surface(&ctx->stream.nvmm_bufs[idx], idx, w, h) == 0) {
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
    req.FrameCaptureRequestId = 0; /* 0 = not a reprocessing request (JXD docs) */
    memcpy(req.FrameControlProps, ctx->default_ctrl_props,
           sizeof(req.FrameControlProps));
    /* Crop region = full sensor area (required for ISP scaler) */
    req.crop_rect.left = 0;
    req.crop_rect.top = 0;
    req.crop_rect.right = 2592;
    req.crop_rect.bottom = 1944;

    ctx->frame_done = 0;
    ctx->shutter_timestamp = 0;

    NvError err = fn_FrameCaptureRequest(ctx->core_handle, &req);

    if (frame_num < 5)
        FLOG("FrameCaptureRequest frame %u -> %d\n", frame_num, err);

    if (err != NvSuccess) {
        FLOG("FrameCaptureRequest FAILED: %d\n", err);
        return -EIO;
    }

    /* Wait for Shutter + CompletedBuffer (max 1000ms) */
    int wait_ms = 0;
    while ((!ctx->frame_done || !ctx->shutter_timestamp) && wait_ms < 1000) {
        usleep(1000);
        wait_ms++;
    }

    FLOG("frame %u done=%d shutter=%d wait=%dms\n",
         frame_num, ctx->frame_done, (int)(ctx->shutter_timestamp != 0), wait_ms);

    /* Save first frame from our allocated NvMM buffer via NvRmMemRead */
    if (frame_num == 0 && ctx->frame_done && fn_NvRmMemRead) {
        NvRmSurface *s0 = &nvmm->Payload.Surfaces.Surfaces[0];
        NvU32 size0 = s0->Pitch * s0->Height;
        uint8_t *buf = (uint8_t *)malloc(size0);
        if (buf) {
            fn_NvRmMemRead(s0->hMem, 0, buf, size0);
            int non_init = 0;
            NvU32 check = size0 > 65536 ? 65536 : size0;
            for (NvU32 i = 0; i < check; i++) {
                if (buf[i] != 0x10 && buf[i] != 0x80 && buf[i] != 0) non_init++;
            }
            FLOG("frame0: Y plane %u bytes non_init=%d (first: %02x %02x %02x %02x)\n",
                 size0, non_init, buf[0], buf[1], buf[2], buf[3]);

            FILE *f = fopen("/data/frame0_nvmap.raw", "wb");
            if (f) {
                fwrite(buf, 1, size0, f);
                fclose(f);
                FLOG("frame0: saved %u bytes\n", size0);
            }
            free(buf);
        }
    }

    /* Return result to framework */
    camera3_capture_result_t result;
    memset(&result, 0, sizeof(result));
    result.frame_number = frame_num;
    result.num_output_buffers = 1;

    camera3_stream_buffer_t result_buf = out_copy;
    result_buf.status = ctx->frame_done ? CAMERA3_BUFFER_STATUS_OK : CAMERA3_BUFFER_STATUS_ERROR;
    result_buf.release_fence = -1;
    result.output_buffers = &result_buf;
    result.result = build_result_meta(ctx->shutter_timestamp);

    ctx->callback_ops->process_capture_result(ctx->callback_ops, &result);
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

#define NUM_CAMERAS 2

static int g_detect_done;
static camera_metadata_t *g_static_info[NUM_CAMERAS];

static int hal3_get_number_of_cameras(void)
{
    if (!g_detect_done) {
        g_logf = fopen("/data/camera_hal.log", "a");
        FLOG("=== Camera HAL3 init ===\n");

        if (load_libs() != 0) {
            FLOG("FATAL: load_libs failed\n");
            g_detect_done = 1;
            return 0;
        }

        if (fn_DeviceDetect) {
            FLOG("NvMMCameraDeviceDetect...\n");
            fn_DeviceDetect();
            FLOG("NvMMCameraDeviceDetect done\n");
        }

        for (int i = 0; i < NUM_CAMERAS; i++) {
            g_static_info[i] = build_static_info(i);
            FLOG("static_info[%d]=%p\n", i, g_static_info[i]);
        }
        g_detect_done = 1;
    }
    return NUM_CAMERAS;
}

static int hal3_get_camera_info(int camera_id, struct camera_info *info)
{
    if (!g_detect_done) hal3_get_number_of_cameras();
    if (camera_id < 0 || camera_id >= NUM_CAMERAS) return -EINVAL;
    if (!g_static_info[camera_id]) return -ENODEV;

    info->facing = (camera_id == 0) ? CAMERA_FACING_BACK : CAMERA_FACING_FRONT;
    info->orientation = (camera_id == 0) ? 90 : 270;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_0;
    info->static_camera_characteristics = g_static_info[camera_id];
    FLOG("get_camera_info: id=%d facing=%d orient=%d OK\n",
         camera_id, info->facing, info->orientation);
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
    FLOG("device_open: id=%s camera_id=%d\n", id, camera_id);
    if (camera_id < 0 || camera_id >= NUM_CAMERAS) return -ENODEV;

    if (load_libs() != 0) return -ENODEV;

    camera3_device_t *dev = (camera3_device_t *)calloc(1, sizeof(*dev));
    struct camera_context *ctx = (struct camera_context *)calloc(1, sizeof(*ctx));
    if (!dev || !ctx) { free(dev); free(ctx); return -ENOMEM; }

    ctx->camera_id = camera_id;

    /* Open NvCameraCore — GUID 0=IMX179 rear, 1=OV5693 front */
    NvError err = fn_Open(&ctx->core_handle, camera_id);
    FLOG("NvCameraCore_Open(%d) -> %d handle=%p\n", camera_id, err, ctx->core_handle);
    if (err != NvSuccess) {
        ALOGE("NvCameraCore_Open failed: %d", err);
        free(ctx); free(dev);
        return -EIO;
    }

    fn_Callback(ctx->core_handle, ctx, nvcamera_callback);

    /* Get default 3A/control properties — required for FrameCaptureRequest */
    if (fn_GetDefaultCtrl) {
        memset(ctx->default_ctrl_props, 0, sizeof(ctx->default_ctrl_props));
        NvError ctrl_err = fn_GetDefaultCtrl(ctx->core_handle, 1 /* PREVIEW */,
                                              ctx->default_ctrl_props);
        FLOG("GetDefaultControlProperties -> %d\n", ctrl_err);
    }

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
