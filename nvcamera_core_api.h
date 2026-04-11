/*
 * Minimal NvCameraCore API declarations for camera HAL.
 * Types extracted from JXD vendor_nvidia_jxd_src headers.
 * We dlopen() libnvmm_camera_v3.so at runtime.
 */

#ifndef NVCAMERA_CORE_API_H
#define NVCAMERA_CORE_API_H

#include <stdint.h>
#include <stddef.h>

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

/* -------------------------------------------------------------------------- */
/* NvRmSurface / NvMMBuffer — real structures for buffer passing              */
/* -------------------------------------------------------------------------- */

/* Color format (from nvcolor.h) — subset we need */
typedef NvU32 NvColorFormat;

/* Surface layout */
typedef enum {
    NvRmSurfaceLayout_Pitch = 1,
    NvRmSurfaceLayout_Tiled = 2,
    NvRmSurfaceLayout_Blocklinear = 3,
} NvRmSurfaceLayout;

/* Display scan format */
typedef enum {
    NvDisplayScanFormat_Progressive = 0,
    NvDisplayScanFormat_Interlaced = 1,
} NvDisplayScanFormat;

/* Opaque memory handle */
typedef void *NvRmMemHandle;
typedef void *NvRmDeviceHandle;

/* Memory kind (for blocklinear) */
typedef NvU32 NvRmMemKind;

/*
 * NvRmSurface — describes a single surface plane (from nvrm_surface.h).
 * Must match binary layout of stock blob (44 bytes on ARM32).
 */
typedef struct NvRmSurfaceRec {
    NvU32               Width;
    NvU32               Height;
    NvColorFormat       ColorFormat;
    NvRmSurfaceLayout   Layout;
    NvU32               Pitch;
    NvRmMemHandle       hMem;
    NvU32               Offset;
    void               *pBase;
    NvRmMemKind         Kind;
    NvU32               BlockHeightLog2;
    NvDisplayScanFormat DisplayScanFormat;
    NvU32               SecondFieldOffset;
} NvRmSurface;

#define NVMMSURFACEDESCRIPTOR_MAX_SURFACES 3

/* Fences (opaque) */
typedef struct NvMMSurfaceFencesRec NvMMSurfaceFences;

/* Display resolution */
typedef struct {
    NvU16 Width;
    NvU16 Height;
} NvMMDisplayResolution;

/* Surface descriptor — array of surfaces + metadata */
typedef struct {
    NvRmSurface         Surfaces[NVMMSURFACEDESCRIPTOR_MAX_SURFACES];
    NvRect              CropRect;
    NvMMDisplayResolution DispRes;
    NvU32               PhysicalAddress[NVMMSURFACEDESCRIPTOR_MAX_SURFACES];
    NvS32               SurfaceCount;
    NvU16               ViewId;
    NvBool              Empty;
    NvMMSurfaceFences  *fences;
} NvMMSurfaceDescriptor;

/* Payload type */
typedef enum {
    NvMMPayloadType_None = 0,
    NvMMPayloadType_SurfaceArray,
    NvMMPayloadType_MemHandle,
    NvMMPayloadType_MemPointer,
} NvMMPayloadType;

/*
 * Payload metadata (from nvmm_buffertype.h).
 * Contains timestamp, flags, and metadata union (128 bytes total).
 */
/*
 * PayloadMetadata size verified via Ghidra RE of stock camera.tegra.so:
 * InitializeNvMMBufferWithANB accesses Payload.Surfaces at offset 0x50 in NvMMBuffer,
 * meaning PayloadInfo = 64 bytes total (not 128 as in JXD headers).
 * MetaDataUnion = 64 - 8(TimeStamp) - 4(Flags) - 4(Type) = 48 bytes.
 */
typedef struct {
    NvU64 TimeStamp;          /* 8 bytes */
    NvU32 BufferFlags;        /* 4 bytes */
    NvU32 BufferMetaDataType; /* 4 bytes (enum) */
    NvU8  MetaDataUnion[48];  /* 48 bytes (MIUI binary, not 112 as in JXD) */
} NvMMPayloadMetadata;

/* Memory reference (for non-surface payloads) */
typedef struct {
    NvU32 data[16]; /* opaque */
} NvMMMemReference;

/*
 * NvMMBuffer — the buffer structure passed to NvCameraCore.
 * Must match binary layout of stock blob.
 */
typedef struct NvMMBufferRec {
    NvU32               StructSize;
    NvU32               BufferID;
    void               *pClientContext;
    NvMMPayloadType     PayloadType;
    NvMMPayloadMetadata PayloadInfo;
    union {
        NvMMSurfaceDescriptor Surfaces;
        NvMMMemReference      Ref;
    } Payload;
    void               *pCore;
} NvMMBuffer;

/* -------------------------------------------------------------------------- */
/* nvgr — NVIDIA gralloc surface extraction (from libnvgr.so via dlsym)       */
/* -------------------------------------------------------------------------- */

typedef void (*pfn_nvgr_get_surfaces)(
    void *handle,               /* buffer_handle_t */
    const NvRmSurface **surfs,
    size_t *surfCount);

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

/* NvSize (from nvcommon.h) */
typedef struct {
    NvS32 width;
    NvS32 height;
} NvSize;

/* Sensor mode (from nvmm_camera_types.h) — full struct, 16 bytes */
typedef struct {
    NvSize Resolution;
    NvF32 FrameRate;
    NvColorFormat ColorFormat;
} NvMMCameraSensorMode;

/*
 * Frame capture request (from nvcamera_core.h)
 * Simplified — full version has FrameControlProps etc.
 */
/*
 * Actual sizes measured on MIUI 4.4 blobs via pattern-fill test:
 *   NvCamProperty_Public_Controls = 820 bytes
 *   NvCamProperty_Public_Dynamic  = unknown (PartialResult info_size=57744 includes more)
 * These MUST match the stock blob layout exactly.
 */
#define NVCAM_CONTROLS_SIZE  820   /* measured via pattern-fill on MIUI 4.4 blobs */
#define NVCAM_DYNAMIC_SIZE   57732 /* info_size(57744) - FrameNumber(4) - Num(4) - pp(4) */

typedef struct {
    NvU32 FrameNumber;
    uint8_t FrameControlProps[NVCAM_CONTROLS_SIZE];
    NvMMBuffer *pInputBuffer;
    NvU32 NumOfOutputBuffers;
    NvMMBuffer **ppOutputBuffers;
    NvU64 FrameCaptureRequestId;
    NvRect crop_rect;
} NvCameraCoreFrameCaptureRequest;

/* Frame capture result — passed in CompletedBuffer callback info */
typedef struct {
    NvU32 FrameNumber;
    uint8_t FrameDynamicProps[NVCAM_DYNAMIC_SIZE];
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
