/*
 * No-op vendor ops for AOSP / LineageOS CameraService.
 * Standard CameraService has no extra tag requirements.
 */

#include "vendor_ops.h"

static void noop_add_static(camera_metadata_t *m, meta_add_fn add)
{
    (void)m; (void)add;
}

static void noop_add_result(camera_metadata_t *m, meta_add_fn add,
                            uint32_t frame_number)
{
    (void)m; (void)add; (void)frame_number;
}

static const struct vendor_ops noop_ops = {
    .add_static_metadata  = noop_add_static,
    .add_result_metadata  = noop_add_result,
};

const struct vendor_ops *vendor_ops_get(void)
{
    return &noop_ops;
}
