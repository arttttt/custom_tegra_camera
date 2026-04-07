/*
 * Minimal NvCameraCore API declarations for camera HAL.
 * Types extracted from JXD vendor_nvidia_jxd_src headers.
 * We dlopen() libnvmm_camera_v3.so at runtime.
 */

#ifndef NVCAMERA_CORE_API_H
#define NVCAMERA_CORE_API_H

#include <stdint.h>

/* Primitive types (from nvcommon.h) */
typedef uint8_t  NvU8;
typedef uint16_t NvU16;
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef int32_t  NvS32;
typedef float    NvF32;
typedef NvU8     NvBool;

#define NV_TRUE  1
#define NV_FALSE 0

/* Error codes (from nverror.h) */
typedef enum {
    NvSuccess = 0,
    NvError_NotImplemented = 1,
    NvError_NotSupported = 2,
    NvError_NotInitialized = 3,
    NvError_BadParameter = 4,
    NvError_Timeout = 5,
    NvError_InsufficientMemory = 6,
    NvError_BadValue = 0x30004,
    NvError_Force32 = 0x7FFFFFFF,
} NvError;

/* Rect (from nvcommon.h) */
typedef struct {
    NvS32 left;
    NvS32 top;
    NvS32 right;
    NvS32 bottom;
} NvRect;

/* Opaque handle */
typedef struct NvCameraCoreContextRec *NvCameraCoreHandle;

/*
 * NvMMBuffer — simplified. The real structure is complex (surfaces, fences).
 * For our minimal HAL we pass opaque pointers from gralloc → NvCameraCore.
 * Full definition will be needed when we wire up actual buffer passing.
 */
typedef struct NvMMBufferRec NvMMBuffer;

/*
 * Camera core events (from nvcamera_core.h)
 * NvMMEventCamera_EventOffset = 0x20000
 */
typedef enum {
    NvCameraCoreEvent_Shutter         = 0x20000,
    NvCameraCoreEvent_PartialResultReady,
    NvCameraCoreEvent_CompletedBuffer,
    NvCameraCoreEvent_Error,
    NvCameraCoreEvent_Force32 = 0x7FFFFFFF,
} NvCameraCoreEvent;

/* Sensor mode (simplified from nvmm_camera_types.h) */
typedef struct {
    NvS32 width;
    NvS32 height;
    /* Additional fields exist but not needed for minimal HAL */
} NvMMCameraSensorMode;

/*
 * Frame capture request (from nvcamera_core.h)
 * Simplified — full version has FrameControlProps etc.
 */
typedef struct {
    NvU32 FrameNumber;
    /* NvCamProperty_Public_Controls FrameControlProps; -- opaque for now */
    uint8_t FrameControlProps[4096]; /* opaque blob, zero-filled */
    NvMMBuffer *pInputBuffer;
    NvU32 NumOfOutputBuffers;
    NvMMBuffer **ppOutputBuffers;
    NvU64 FrameCaptureRequestId;
    NvRect crop_rect;
} NvCameraCoreFrameCaptureRequest;

/* Frame capture result */
typedef struct {
    NvU32 FrameNumber;
    uint8_t FrameDynamicProps[4096]; /* opaque blob */
    NvU32 NumCompletedOutputBuffers;
    NvMMBuffer **ppOutputBuffers;
} NvCameraCoreFrameCaptureResult;

/* Shutter event info */
typedef struct {
    NvU64 Timestamp;
    NvU32 FrameNumber;
} NvCameraCoreShutterEventInfo;

/*
 * Callback function type.
 * Called by NvCameraCore when events occur (shutter, buffer complete, error).
 */
typedef NvError (*NvCameraCoreEventCallbackFunction)(
    void *pContext,
    NvU32 EventType,
    NvU32 EventInfoSize,
    void *pEventInfo);

/*
 * NvCameraCore function pointer types — resolved via dlsym()
 */
typedef NvError (*pfn_NvCameraCore_Open)(
    NvCameraCoreHandle *phCameraCore, NvU64 ImagerGUID);

typedef void (*pfn_NvCameraCore_Close)(
    NvCameraCoreHandle hCameraCore);

typedef NvError (*pfn_NvCameraCore_CallbackFunction)(
    NvCameraCoreHandle hCameraCore,
    void *pClientContext,
    NvCameraCoreEventCallbackFunction CallbackFunction);

typedef NvError (*pfn_NvCameraCore_GetStaticProperties)(
    NvCameraCoreHandle hCameraCore,
    void *pOutStaticProps);

typedef NvError (*pfn_NvCameraCore_GetDefaultControlProperties)(
    NvCameraCoreHandle hCameraCore,
    NvU32 UseCase,
    void *pOutDefaultControlProps);

typedef NvError (*pfn_NvCameraCore_SetSensorMode)(
    NvCameraCoreHandle hCameraCore,
    NvMMCameraSensorMode RequestedSensorMode);

typedef NvError (*pfn_NvCameraCore_FrameCaptureRequest)(
    NvCameraCoreHandle hCameraCore,
    NvCameraCoreFrameCaptureRequest *pFrameRequest);

typedef NvError (*pfn_NvCameraCore_Flush)(
    NvCameraCoreHandle hCameraCore);

typedef NvBool (*pfn_NvMMCameraDeviceDetect)(void);

/*
 * Sensor GUIDs (from nvodm_imager_guid.h)
 * NV_ODM_GUID('a','b','c','d','e','f','g','h') =
 *   ((NvU64)'a'<<56 | (NvU64)'b'<<48 | ... | (NvU64)'h')
 */
#define NV_ODM_GUID(a,b,c,d,e,f,g,h) \
    ((NvU64)(a)<<56 | (NvU64)(b)<<48 | (NvU64)(c)<<40 | (NvU64)(d)<<32 | \
     (NvU64)(e)<<24 | (NvU64)(f)<<16 | (NvU64)(g)<<8  | (NvU64)(h))

#define SENSOR_BAYER_IMX179_GUID       NV_ODM_GUID('s','_','I','M','X','1','7','9')
#define SENSOR_BAYER_OV5693_FRONT_GUID NV_ODM_GUID('s','O','V','5','6','9','3','f')

#endif /* NVCAMERA_CORE_API_H */
