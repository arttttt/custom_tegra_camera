/*
 * MIUI CameraService vendor ops.
 *
 * Xiaomi's libcameraservice.so calls CameraMetadata::find() on tags that don't
 * exist in AOSP. If the tag is missing, find() returns NULL → SEGV.
 *
 * This provides safe stub values so CameraService doesn't crash.
 * Remove -DMIUI_CAMERA_SERVICE from Android.mk to switch to noop.
 */

#include "vendor_ops.h"
#include "miui_camera_metadata_tags.h"

static void miui_add_static(camera_metadata_t *m, meta_add_fn add)
{
    /* setTimestampMultFactor reads maxFrameDuration from static info */
    int64_t max_frame_dur = 300000000LL; /* 300ms */
    add(m, MIUI_SENSOR_INFO_MAX_FRAME_DUR, &max_frame_dur, 1);

    int32_t white_level = 4095; /* 12-bit sensor */
    add(m, MIUI_SENSOR_INFO_WHITE_LEVEL, &white_level, 1);
}

static void miui_add_request(camera_metadata_t *m, meta_add_fn add)
{
    /*
     * setTimestampMultFactor crashes if it can't find frame duration tags.
     * Add both sensor.frameDuration and sensor.info.maxFrameDuration.
     */
    int64_t frame_dur = 33333333LL; /* 30fps */
    add(m, MIUI_SENSOR_FRAME_DURATION, &frame_dur, 1);

    int64_t max_frame_dur = 300000000LL;
    add(m, MIUI_SENSOR_INFO_MAX_FRAME_DUR, &max_frame_dur, 1);
}

static void miui_add_result(camera_metadata_t *m, meta_add_fn add,
                            uint32_t frame_number)
{
    (void)m; (void)add; (void)frame_number;
}

static const struct vendor_ops miui_ops = {
    .add_static_metadata  = miui_add_static,
    .add_request_metadata = miui_add_request,
    .add_result_metadata  = miui_add_result,
};

const struct vendor_ops *vendor_ops_get(void)
{
    return &miui_ops;
}
