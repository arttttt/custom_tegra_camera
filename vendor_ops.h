/*
 * Vendor-specific camera metadata operations.
 *
 * Different CameraService builds (MIUI, AOSP, LOS) may require extra metadata
 * tags in static characteristics or per-frame results. This interface isolates
 * those requirements from the core HAL logic.
 *
 * Select implementation at build time via Android.mk (MIUI_CAMERA_SERVICE flag).
 */

#ifndef VENDOR_OPS_H
#define VENDOR_OPS_H

#include <stdint.h>

/* Forward declaration — actual type comes from system/camera_metadata.h */
typedef struct camera_metadata camera_metadata_t;
typedef int (*meta_add_fn)(camera_metadata_t *, uint32_t, const void *, size_t);

struct vendor_ops {
    /* Append vendor-specific entries to static camera characteristics */
    void (*add_static_metadata)(camera_metadata_t *m, meta_add_fn add);

    /* Append vendor-specific entries to default request settings */
    void (*add_request_metadata)(camera_metadata_t *m, meta_add_fn add);

    /* Append vendor-specific entries to per-frame result metadata */
    void (*add_result_metadata)(camera_metadata_t *m, meta_add_fn add,
                                uint32_t frame_number);
};

#ifdef __cplusplus
extern "C"
#endif
const struct vendor_ops *vendor_ops_get(void);

#endif /* VENDOR_OPS_H */
