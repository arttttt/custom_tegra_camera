/*
 * MIUI 4.4 camera_metadata tag IDs for Xiaomi Mi Pad (mocha).
 *
 * Dumped from device's libcamera_metadata.so via get_camera_metadata_tag_name().
 *
 * Differences vs AOSP 7.1.2 (which we compile against):
 *   1. GEOMETRIC section (6) exists in 4.4, removed in 5.0+
 *      → all sections from HOT_PIXEL (7) onward are shifted +1
 *   2. highFpsRecordingMode (0x10012) added in CONTROL section
 *      → static control tags (0x10013+) and dynamic state tags shifted +1
 *   3. Xiaomi adds beauty(25), watermark(26), morpho(27) sections at the end
 *
 * Sections 0–5 (colorCorrection through flash.info) are unchanged.
 */

#ifndef MIUI_CAMERA_METADATA_TAGS_H
#define MIUI_CAMERA_METADATA_TAGS_H

/*
 * Section 1: android.control (0x10000)
 *
 * Dynamic tags 0x10000–0x10011 match AOSP.
 * 0x10012 = highFpsRecordingMode (Xiaomi/NVIDIA addition).
 * Static and state tags start at 0x10013 (+1 vs AOSP).
 */
#define MIUI_CONTROL_AE_MODE                0x10003
#define MIUI_CONTROL_AWB_MODE               0x1000b
#define MIUI_CONTROL_CAPTURE_INTENT         0x1000d
#define MIUI_CONTROL_MODE                   0x1000f

#define MIUI_CONTROL_AE_AVAIL_ANTIBANDING   0x10013
#define MIUI_CONTROL_AE_AVAIL_MODES         0x10014
#define MIUI_CONTROL_AE_AVAIL_FPS_RANGES    0x10015
#define MIUI_CONTROL_AE_COMP_RANGE          0x10016
#define MIUI_CONTROL_AE_COMP_STEP           0x10017
#define MIUI_CONTROL_AF_AVAIL_MODES         0x10018
#define MIUI_CONTROL_AVAIL_EFFECTS          0x10019
#define MIUI_CONTROL_AVAIL_SCENE_MODES      0x1001a
#define MIUI_CONTROL_AVAIL_VSTAB_MODES      0x1001b
#define MIUI_CONTROL_AWB_AVAIL_MODES        0x1001c
#define MIUI_CONTROL_MAX_REGIONS            0x1001d
#define MIUI_CONTROL_SCENE_MODE_OVERRIDES   0x1001e

#define MIUI_CONTROL_AE_STATE               0x10020
#define MIUI_CONTROL_AF_STATE               0x10021
#define MIUI_CONTROL_AWB_STATE              0x10023

/* Section 5: android.flash.info (0x50000) — unchanged */
#define MIUI_FLASH_INFO_AVAILABLE           0x50000

/* Section 9: android.jpeg (0x90000) — was section 8 in 7.1.2 */
#define MIUI_JPEG_ORIENTATION               0x90003
#define MIUI_JPEG_QUALITY                   0x90004
#define MIUI_JPEG_THUMB_QUALITY             0x90005
#define MIUI_JPEG_THUMB_SIZE                0x90006
#define MIUI_JPEG_AVAIL_THUMB_SIZES         0x90007
#define MIUI_JPEG_MAX_SIZE                  0x90008

/* Section 10: android.lens (0xa0000) — was section 9 in 7.1.2 */
#define MIUI_LENS_FACING                    0xa0005

/* Section 11: android.lens.info (0xb0000) — was section 10 in 7.1.2 */
#define MIUI_LENS_INFO_AVAIL_APERTURES      0xb0000
#define MIUI_LENS_INFO_AVAIL_FOCAL_LENGTHS  0xb0002
#define MIUI_LENS_INFO_HYPERFOCAL_DIST      0xb0006
#define MIUI_LENS_INFO_MIN_FOCUS_DISTANCE   0xb0007

/* Section 14: android.request (0xe0000) — was section 13 in 7.1.2 */
#define MIUI_REQUEST_MAX_NUM_OUTPUT_STREAMS 0xe0006

/* Section 15: android.scaler (0xf0000) — was section 14 in 7.1.2 */
#define MIUI_SCALER_CROP_REGION             0xf0000
#define MIUI_SCALER_AVAIL_FORMATS           0xf0001
#define MIUI_SCALER_AVAIL_JPEG_MIN_DUR      0xf0002
#define MIUI_SCALER_AVAIL_JPEG_SIZES        0xf0003
#define MIUI_SCALER_AVAIL_MAX_DIGITAL_ZOOM  0xf0004
#define MIUI_SCALER_AVAIL_PROC_MIN_DUR      0xf0005
#define MIUI_SCALER_AVAIL_PROC_SIZES        0xf0006

/* Section 16: android.sensor (0x100000) — was section 15 in 7.1.2 */
#define MIUI_SENSOR_EXPOSURE_TIME           0x100000
#define MIUI_SENSOR_FRAME_DURATION          0x100001
#define MIUI_SENSOR_SENSITIVITY             0x100002
#define MIUI_SENSOR_ORIENTATION             0x10000d
#define MIUI_SENSOR_TIMESTAMP               0x100010

/* Section 17: android.sensor.info (0x110000) — was section 16 in 7.1.2 */
#define MIUI_SENSOR_INFO_ACTIVE_ARRAY       0x110000
#define MIUI_SENSOR_INFO_SENSITIVITY_RANGE  0x110001
#define MIUI_SENSOR_INFO_EXPOSURE_RANGE     0x110003
#define MIUI_SENSOR_INFO_MAX_FRAME_DUR      0x110004
#define MIUI_SENSOR_INFO_PHYSICAL_SIZE      0x110005
#define MIUI_SENSOR_INFO_PIXEL_ARRAY        0x110006
#define MIUI_SENSOR_INFO_WHITE_LEVEL        0x110007

/* Section 20: android.statistics.info (0x140000) — was section 19 in 7.1.2 */
#define MIUI_STATS_INFO_AVAIL_FACE_DETECT   0x140000
#define MIUI_STATS_INFO_MAX_FACE_COUNT      0x140002

/* Section 23: android.info (0x170000) — was section 22 in 7.1.2 */
#define MIUI_INFO_HW_LEVEL                  0x170000

#endif /* MIUI_CAMERA_METADATA_TAGS_H */
