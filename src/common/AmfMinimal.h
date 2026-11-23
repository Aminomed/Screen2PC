#pragma once
// Minimal MIT-licensed AMD Advanced Media Framework (AMF) C-ABI definitions
// Derived from GPUOpen-LibrariesAndSDKs/AMF public headers for dynamic amfrt64.dll loading.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <cstddef>
#include <cstdint>

namespace s2p::amf_abi {

using amf_int64  = int64_t;
using amf_int32  = int32_t;
using amf_int16  = int16_t;
using amf_int8   = int8_t;
using amf_uint64 = uint64_t;
using amf_uint32 = uint32_t;
using amf_uint16 = uint16_t;
using amf_uint8  = uint8_t;
using amf_size   = size_t;
using amf_handle = void*;
using amf_double = double;
using amf_float  = float;
using amf_bool   = bool;
using amf_long   = long;
using amf_int    = int;
using amf_ulong  = unsigned long;
using amf_uint   = unsigned int;
using amf_pts    = amf_int64;

#define S2P_AMF_STD_CALL   __stdcall
#define S2P_AMF_CDECL_CALL __cdecl

#define S2P_AMF_MAKE_FULL_VERSION(MAJ, MIN, REL, BLD) \
    ((static_cast<amf_uint64>(MAJ) << 48ULL) |        \
     (static_cast<amf_uint64>(MIN) << 32ULL) |        \
     (static_cast<amf_uint64>(REL) << 16ULL) |        \
     static_cast<amf_uint64>(BLD))

constexpr amf_uint64 kAmfMinApiVersion = S2P_AMF_MAKE_FULL_VERSION(1, 4, 9, 0);
constexpr amf_uint64 kAmfSdkVersion    = S2P_AMF_MAKE_FULL_VERSION(1, 4, 35, 0);

struct AMFRect {
    amf_int32 left, top, right, bottom;
};

struct AMFSize {
    amf_int32 width, height;
};

struct AMFPoint {
    amf_int32 x, y;
};

struct AMFRate {
    amf_uint32 num, den;
};

struct AMFRatio {
    amf_uint32 num, den;
};

#pragma pack(push, 1)
struct AMFColor {
    union {
        struct { amf_uint8 r, g, b, a; };
        amf_uint32 rgba;
    };
};
#pragma pack(pop)

struct AMFGuid {
    amf_uint32 data1;
    amf_uint16 data2;
    amf_uint16 data3;
    amf_uint8  data41, data42, data43, data44, data45, data46, data47, data48;
};

enum AMF_RESULT : int {
    AMF_OK = 0,
    AMF_FAIL = 1,
    AMF_UNEXPECTED = 2,
    AMF_ACCESS_DENIED = 3,
    AMF_INVALID_ARG = 4,
    AMF_OUT_OF_RANGE = 5,
    AMF_OUT_OF_MEMORY = 6,
    AMF_INVALID_POINTER = 7,
    AMF_NO_INTERFACE = 8,
    AMF_NOT_IMPLEMENTED = 9,
    AMF_NOT_SUPPORTED = 10,
    AMF_NOT_FOUND = 11,
    AMF_ALREADY_INITIALIZED = 12,
    AMF_NOT_INITIALIZED = 13,
    AMF_INVALID_FORMAT = 14,
    AMF_WRONG_STATE = 15,
    AMF_FILE_NOT_OPEN = 16,
    AMF_NO_DEVICE = 17,
    AMF_DIRECTX_FAILED = 18,
    AMF_OPENCL_FAILED = 19,
    AMF_GLX_FAILED = 20,
    AMF_XV_FAILED = 21,
    AMF_ALSA_FAILED = 22,
    AMF_EOF = 23,
    AMF_REPEAT = 24,
    AMF_INPUT_FULL = 25,
    AMF_RESOLUTION_CHANGED = 26,
    AMF_RESOLUTION_UPDATED = 27,
    AMF_INVALID_DATA_TYPE = 28,
    AMF_INVALID_RESOLUTION = 29,
    AMF_CODEC_NOT_SUPPORTED = 30,
    AMF_SURFACE_FORMAT_NOT_SUPPORTED = 31,
    AMF_SURFACE_MUST_BE_SHARED = 32,
    AMF_DECODER_NOT_PRESENT = 33,
    AMF_DECODER_SURFACE_ALLOCATION_FAILED = 34,
    AMF_DECODER_NO_FREE_SURFACES = 35,
    AMF_ENCODER_NOT_PRESENT = 36,
    AMF_DEM_ERROR = 37,
    AMF_DEM_PROPERTY_READONLY = 38,
    AMF_DEM_REMOTE_DISPLAY_CREATE_FAILED = 39,
    AMF_DEM_START_ENCODING_FAILED = 40,
    AMF_DEM_QUERY_OUTPUT_FAILED = 41,
    AMF_TAN_CLIPPING_WAS_REQUIRED = 42,
    AMF_TAN_UNSUPPORTED_VERSION = 43,
    AMF_NEED_MORE_INPUT = 44
};

struct AMFInterface;
struct AMFPropertyStorage;
struct AMFPropertyStorageObserver;
struct AMFData;
struct AMFBuffer;
struct AMFBufferObserver;
struct AMFSurface;
struct AMFSurfaceObserver;
struct AMFPlane;
struct AMFAudioBuffer;
struct AMFComponent;
struct AMFContext;
struct AMFCompute;
struct AMFComputeFactory;
struct AMFComputeDevice;
struct AMFIOCaps;
struct AMFCaps;
struct AMFDataAllocatorCB;
struct AMFComponentOptimizationCallback;
struct AMFDebug;
struct AMFTrace;
struct AMFPrograms;
struct AMFFactory;

enum AMF_VARIANT_TYPE : int {
    AMF_VARIANT_EMPTY     = 0,
    AMF_VARIANT_BOOL      = 1,
    AMF_VARIANT_INT64     = 2,
    AMF_VARIANT_DOUBLE    = 3,
    AMF_VARIANT_RECT      = 4,
    AMF_VARIANT_SIZE      = 5,
    AMF_VARIANT_POINT     = 6,
    AMF_VARIANT_RATE      = 7,
    AMF_VARIANT_RATIO     = 8,
    AMF_VARIANT_COLOR     = 9,
    AMF_VARIANT_STRING    = 10,
    AMF_VARIANT_WSTRING   = 11,
    AMF_VARIANT_INTERFACE = 12
};

struct AMFVariantStruct {
    AMF_VARIANT_TYPE type;
    union {
        amf_bool      boolValue;
        amf_int64     int64Value;
        amf_double    doubleValue;
        char*         stringValue;
        wchar_t*      wstringValue;
        AMFInterface* pInterface;
        AMFRect       rectValue;
        AMFSize       sizeValue;
        AMFPoint      pointValue;
        AMFRate       rateValue;
        AMFRatio      ratioValue;
        AMFColor      colorValue;
    };
};

struct AMFPropertyInfo;

enum AMF_DATA_TYPE : int {
    AMF_DATA_BUFFER = 0,
    AMF_DATA_SURFACE = 1,
    AMF_DATA_AUDIO_BUFFER = 2
};

enum AMF_MEMORY_TYPE : int {
    AMF_MEMORY_UNKNOWN = 0,
    AMF_MEMORY_HOST = 1,
    AMF_MEMORY_DX9 = 2,
    AMF_MEMORY_DX11 = 3,
    AMF_MEMORY_OPENCL = 4,
    AMF_MEMORY_OPENGL = 5,
    AMF_MEMORY_XV = 6,
    AMF_MEMORY_GRALLOC = 7,
    AMF_MEMORY_COMPUTE_FOR_DX9 = 8,
    AMF_MEMORY_COMPUTE_FOR_DX11 = 9
};

enum AMF_DX_VERSION : int {
    AMF_DX9 = 90,
    AMF_DX9_EX = 91,
    AMF_DX11_0 = 110,
    AMF_DX11_1 = 111
};

enum AMF_SURFACE_FORMAT : int {
    AMF_SURFACE_UNKNOWN = 0,
    AMF_SURFACE_NV12 = 1,
    AMF_SURFACE_YV12 = 2,
    AMF_SURFACE_BGRA = 3,
    AMF_SURFACE_ARGB = 4,
    AMF_SURFACE_RGBA = 5,
    AMF_SURFACE_GRAY8 = 6,
    AMF_SURFACE_YUV420P = 7,
    AMF_SURFACE_U8V8 = 8,
    AMF_SURFACE_YUY2 = 9,
    AMF_SURFACE_P010 = 10
};

enum AMF_PLANE_TYPE : int {
    AMF_PLANE_UNKNOWN = 0,
    AMF_PLANE_PACKED = 1,
    AMF_PLANE_Y = 2,
    AMF_PLANE_UV = 3,
    AMF_PLANE_U = 4,
    AMF_PLANE_V = 5
};

enum AMF_FRAME_TYPE : int {
    AMF_FRAME_UNKNOWN = -1,
    AMF_FRAME_PROGRESSIVE = 0
};

constexpr AMFGuid kIID_AMFBuffer  = { 0xb04b7248, 0xb6f0, 0x4321, 0xb6, 0x91, 0xba, 0xa4, 0x74, 0x0f, 0x9f, 0xcb };
constexpr AMFGuid kIID_AMFSurface = { 0x3075dbe3, 0x8718, 0x4cfa, 0x86, 0xfb, 0x21, 0x14, 0xc0, 0xa5, 0xa4, 0x51 };

struct AMFInterfaceVtbl {
    amf_long   (S2P_AMF_STD_CALL *Acquire)(AMFInterface* pThis);
    amf_long   (S2P_AMF_STD_CALL *Release)(AMFInterface* pThis);
    AMF_RESULT (S2P_AMF_STD_CALL *QueryInterface)(AMFInterface* pThis, const AMFGuid* interfaceID, void** ppInterface);
};
struct AMFInterface { const AMFInterfaceVtbl* pVtbl; };

struct AMFPlaneVtbl {
    amf_long       (S2P_AMF_STD_CALL *Acquire)(AMFPlane* pThis);
    amf_long       (S2P_AMF_STD_CALL *Release)(AMFPlane* pThis);
    AMF_RESULT     (S2P_AMF_STD_CALL *QueryInterface)(AMFPlane* pThis, const AMFGuid* interfaceID, void** ppInterface);
    AMF_PLANE_TYPE (S2P_AMF_STD_CALL *GetType)(AMFPlane* pThis);
    void*          (S2P_AMF_STD_CALL *GetNative)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetPixelSizeInBytes)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetOffsetX)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetOffsetY)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetWidth)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetHeight)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetHPitch)(AMFPlane* pThis);
    amf_int32      (S2P_AMF_STD_CALL *GetVPitch)(AMFPlane* pThis);
    amf_bool       (S2P_AMF_STD_CALL *IsTiled)(AMFPlane* pThis);
};
struct AMFPlane { const AMFPlaneVtbl* pVtbl; };

struct AMFDataVtbl {
    amf_long        (S2P_AMF_STD_CALL *Acquire)(AMFData* pThis);
    amf_long        (S2P_AMF_STD_CALL *Release)(AMFData* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *QueryInterface)(AMFData* pThis, const AMFGuid* interfaceID, void** ppInterface);
    AMF_RESULT      (S2P_AMF_STD_CALL *SetProperty)(AMFData* pThis, const wchar_t* name, AMFVariantStruct value);
    AMF_RESULT      (S2P_AMF_STD_CALL *GetProperty)(AMFData* pThis, const wchar_t* name, AMFVariantStruct* pValue);
    amf_bool        (S2P_AMF_STD_CALL *HasProperty)(AMFData* pThis, const wchar_t* name);
    amf_size        (S2P_AMF_STD_CALL *GetPropertyCount)(AMFData* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *GetPropertyAt)(AMFData* pThis, amf_size index, wchar_t* name, amf_size nameSize, AMFVariantStruct* pValue);
    AMF_RESULT      (S2P_AMF_STD_CALL *Clear)(AMFData* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *AddTo)(AMFData* pThis, AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep);
    AMF_RESULT      (S2P_AMF_STD_CALL *CopyTo)(AMFData* pThis, AMFPropertyStorage* pDest, amf_bool deep);
    void            (S2P_AMF_STD_CALL *AddObserver)(AMFData* pThis, AMFPropertyStorageObserver* pObserver);
    void            (S2P_AMF_STD_CALL *RemoveObserver)(AMFData* pThis, AMFPropertyStorageObserver* pObserver);
    AMF_MEMORY_TYPE (S2P_AMF_STD_CALL *GetMemoryType)(AMFData* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *Duplicate)(AMFData* pThis, AMF_MEMORY_TYPE type, AMFData** ppData);
    AMF_RESULT      (S2P_AMF_STD_CALL *Convert)(AMFData* pThis, AMF_MEMORY_TYPE type);
    AMF_RESULT      (S2P_AMF_STD_CALL *Interop)(AMFData* pThis, AMF_MEMORY_TYPE type);
    AMF_DATA_TYPE   (S2P_AMF_STD_CALL *GetDataType)(AMFData* pThis);
    amf_bool        (S2P_AMF_STD_CALL *IsReusable)(AMFData* pThis);
    void            (S2P_AMF_STD_CALL *SetPts)(AMFData* pThis, amf_pts pts);
    amf_pts         (S2P_AMF_STD_CALL *GetPts)(AMFData* pThis);
    void            (S2P_AMF_STD_CALL *SetDuration)(AMFData* pThis, amf_pts duration);
    amf_pts         (S2P_AMF_STD_CALL *GetDuration)(AMFData* pThis);
};
struct AMFData { const AMFDataVtbl* pVtbl; };

struct AMFBufferVtbl {
    amf_long        (S2P_AMF_STD_CALL *Acquire)(AMFBuffer* pThis);
    amf_long        (S2P_AMF_STD_CALL *Release)(AMFBuffer* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *QueryInterface)(AMFBuffer* pThis, const AMFGuid* interfaceID, void** ppInterface);
    AMF_RESULT      (S2P_AMF_STD_CALL *SetProperty)(AMFBuffer* pThis, const wchar_t* name, AMFVariantStruct value);
    AMF_RESULT      (S2P_AMF_STD_CALL *GetProperty)(AMFBuffer* pThis, const wchar_t* name, AMFVariantStruct* pValue);
    amf_bool        (S2P_AMF_STD_CALL *HasProperty)(AMFBuffer* pThis, const wchar_t* name);
    amf_size        (S2P_AMF_STD_CALL *GetPropertyCount)(AMFBuffer* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *GetPropertyAt)(AMFBuffer* pThis, amf_size index, wchar_t* name, amf_size nameSize, AMFVariantStruct* pValue);
    AMF_RESULT      (S2P_AMF_STD_CALL *Clear)(AMFBuffer* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *AddTo)(AMFBuffer* pThis, AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep);
    AMF_RESULT      (S2P_AMF_STD_CALL *CopyTo)(AMFBuffer* pThis, AMFPropertyStorage* pDest, amf_bool deep);
    void            (S2P_AMF_STD_CALL *AddObserver)(AMFBuffer* pThis, AMFPropertyStorageObserver* pObserver);
    void            (S2P_AMF_STD_CALL *RemoveObserver)(AMFBuffer* pThis, AMFPropertyStorageObserver* pObserver);
    AMF_MEMORY_TYPE (S2P_AMF_STD_CALL *GetMemoryType)(AMFBuffer* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *Duplicate)(AMFBuffer* pThis, AMF_MEMORY_TYPE type, AMFData** ppData);
    AMF_RESULT      (S2P_AMF_STD_CALL *Convert)(AMFBuffer* pThis, AMF_MEMORY_TYPE type);
    AMF_RESULT      (S2P_AMF_STD_CALL *Interop)(AMFBuffer* pThis, AMF_MEMORY_TYPE type);
    AMF_DATA_TYPE   (S2P_AMF_STD_CALL *GetDataType)(AMFBuffer* pThis);
    amf_bool        (S2P_AMF_STD_CALL *IsReusable)(AMFBuffer* pThis);
    void            (S2P_AMF_STD_CALL *SetPts)(AMFBuffer* pThis, amf_pts pts);
    amf_pts         (S2P_AMF_STD_CALL *GetPts)(AMFBuffer* pThis);
    void            (S2P_AMF_STD_CALL *SetDuration)(AMFBuffer* pThis, amf_pts duration);
    amf_pts         (S2P_AMF_STD_CALL *GetDuration)(AMFBuffer* pThis);
    AMF_RESULT      (S2P_AMF_STD_CALL *SetSize)(AMFBuffer* pThis, amf_size newSize);
    amf_size        (S2P_AMF_STD_CALL *GetSize)(AMFBuffer* pThis);
    void*           (S2P_AMF_STD_CALL *GetNative)(AMFBuffer* pThis);
    void            (S2P_AMF_STD_CALL *AddObserver_Buffer)(AMFBuffer* pThis, AMFBufferObserver* pObserver);
    void            (S2P_AMF_STD_CALL *RemoveObserver_Buffer)(AMFBuffer* pThis, AMFBufferObserver* pObserver);
};
struct AMFBuffer { const AMFBufferVtbl* pVtbl; };

struct AMFSurfaceVtbl {
    amf_long           (S2P_AMF_STD_CALL *Acquire)(AMFSurface* pThis);
    amf_long           (S2P_AMF_STD_CALL *Release)(AMFSurface* pThis);
    AMF_RESULT         (S2P_AMF_STD_CALL *QueryInterface)(AMFSurface* pThis, const AMFGuid* interfaceID, void** ppInterface);
    AMF_RESULT         (S2P_AMF_STD_CALL *SetProperty)(AMFSurface* pThis, const wchar_t* name, AMFVariantStruct value);
    AMF_RESULT         (S2P_AMF_STD_CALL *GetProperty)(AMFSurface* pThis, const wchar_t* name, AMFVariantStruct* pValue);
    amf_bool           (S2P_AMF_STD_CALL *HasProperty)(AMFSurface* pThis, const wchar_t* name);
    amf_size           (S2P_AMF_STD_CALL *GetPropertyCount)(AMFSurface* pThis);
    AMF_RESULT         (S2P_AMF_STD_CALL *GetPropertyAt)(AMFSurface* pThis, amf_size index, wchar_t* name, amf_size nameSize, AMFVariantStruct* pValue);
    AMF_RESULT         (S2P_AMF_STD_CALL *Clear)(AMFSurface* pThis);
    AMF_RESULT         (S2P_AMF_STD_CALL *AddTo)(AMFSurface* pThis, AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep);
    AMF_RESULT         (S2P_AMF_STD_CALL *CopyTo)(AMFSurface* pThis, AMFPropertyStorage* pDest, amf_bool deep);
    void               (S2P_AMF_STD_CALL *AddObserver)(AMFSurface* pThis, AMFPropertyStorageObserver* pObserver);
    void               (S2P_AMF_STD_CALL *RemoveObserver)(AMFSurface* pThis, AMFPropertyStorageObserver* pObserver);
    AMF_MEMORY_TYPE    (S2P_AMF_STD_CALL *GetMemoryType)(AMFSurface* pThis);
    AMF_RESULT         (S2P_AMF_STD_CALL *Duplicate)(AMFSurface* pThis, AMF_MEMORY_TYPE type, AMFData** ppData);
    AMF_RESULT         (S2P_AMF_STD_CALL *Convert)(AMFSurface* pThis, AMF_MEMORY_TYPE type);
    AMF_RESULT         (S2P_AMF_STD_CALL *Interop)(AMFSurface* pThis, AMF_MEMORY_TYPE type);
    AMF_DATA_TYPE      (S2P_AMF_STD_CALL *GetDataType)(AMFSurface* pThis);
    amf_bool           (S2P_AMF_STD_CALL *IsReusable)(AMFSurface* pThis);
    void               (S2P_AMF_STD_CALL *SetPts)(AMFSurface* pThis, amf_pts pts);
    amf_pts            (S2P_AMF_STD_CALL *GetPts)(AMFSurface* pThis);
    void               (S2P_AMF_STD_CALL *SetDuration)(AMFSurface* pThis, amf_pts duration);
    amf_pts            (S2P_AMF_STD_CALL *GetDuration)(AMFSurface* pThis);
    AMF_SURFACE_FORMAT (S2P_AMF_STD_CALL *GetFormat)(AMFSurface* pThis);
    amf_size           (S2P_AMF_STD_CALL *GetPlanesCount)(AMFSurface* pThis);
    AMFPlane*          (S2P_AMF_STD_CALL *GetPlaneAt)(AMFSurface* pThis, amf_size index);
    AMFPlane*          (S2P_AMF_STD_CALL *GetPlane)(AMFSurface* pThis, AMF_PLANE_TYPE type);
    AMF_FRAME_TYPE     (S2P_AMF_STD_CALL *GetFrameType)(AMFSurface* pThis);
    void               (S2P_AMF_STD_CALL *SetFrameType)(AMFSurface* pThis, AMF_FRAME_TYPE type);
    AMF_RESULT         (S2P_AMF_STD_CALL *SetCrop)(AMFSurface* pThis, amf_int32 x, amf_int32 y, amf_int32 width, amf_int32 height);
    AMF_RESULT         (S2P_AMF_STD_CALL *CopySurfaceRegion)(AMFSurface* pThis, AMFSurface* pDest, amf_int32 dstX, amf_int32 dstY, amf_int32 srcX, amf_int32 srcY, amf_int32 width, amf_int32 height);
    void               (S2P_AMF_STD_CALL *AddObserver_Surface)(AMFSurface* pThis, AMFSurfaceObserver* pObserver);
    void               (S2P_AMF_STD_CALL *RemoveObserver_Surface)(AMFSurface* pThis, AMFSurfaceObserver* pObserver);
};
struct AMFSurface { const AMFSurfaceVtbl* pVtbl; };

struct AMFComponentVtbl {
    amf_long    (S2P_AMF_STD_CALL *Acquire)(AMFComponent* pThis);
    amf_long    (S2P_AMF_STD_CALL *Release)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *QueryInterface)(AMFComponent* pThis, const AMFGuid* interfaceID, void** ppInterface);
    AMF_RESULT  (S2P_AMF_STD_CALL *SetProperty)(AMFComponent* pThis, const wchar_t* name, AMFVariantStruct value);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetProperty)(AMFComponent* pThis, const wchar_t* name, AMFVariantStruct* pValue);
    amf_bool    (S2P_AMF_STD_CALL *HasProperty)(AMFComponent* pThis, const wchar_t* name);
    amf_size    (S2P_AMF_STD_CALL *GetPropertyCount)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetPropertyAt)(AMFComponent* pThis, amf_size index, wchar_t* name, amf_size nameSize, AMFVariantStruct* pValue);
    AMF_RESULT  (S2P_AMF_STD_CALL *Clear)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *AddTo)(AMFComponent* pThis, AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep);
    AMF_RESULT  (S2P_AMF_STD_CALL *CopyTo)(AMFComponent* pThis, AMFPropertyStorage* pDest, amf_bool deep);
    void        (S2P_AMF_STD_CALL *AddObserver)(AMFComponent* pThis, AMFPropertyStorageObserver* pObserver);
    void        (S2P_AMF_STD_CALL *RemoveObserver)(AMFComponent* pThis, AMFPropertyStorageObserver* pObserver);
    amf_size    (S2P_AMF_STD_CALL *GetPropertiesInfoCount)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetPropertyInfoAt)(AMFComponent* pThis, amf_size index, const AMFPropertyInfo** ppInfo);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetPropertyInfo)(AMFComponent* pThis, const wchar_t* name, const AMFPropertyInfo** ppInfo);
    AMF_RESULT  (S2P_AMF_STD_CALL *ValidateProperty)(AMFComponent* pThis, const wchar_t* name, AMFVariantStruct value, AMFVariantStruct* pOutValidated);
    AMF_RESULT  (S2P_AMF_STD_CALL *Init)(AMFComponent* pThis, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height);
    AMF_RESULT  (S2P_AMF_STD_CALL *ReInit)(AMFComponent* pThis, amf_int32 width, amf_int32 height);
    AMF_RESULT  (S2P_AMF_STD_CALL *Terminate)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *Drain)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *Flush)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *SubmitInput)(AMFComponent* pThis, AMFData* pData);
    AMF_RESULT  (S2P_AMF_STD_CALL *QueryOutput)(AMFComponent* pThis, AMFData** ppData);
    AMFContext* (S2P_AMF_STD_CALL *GetContext)(AMFComponent* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *SetOutputDataAllocatorCB)(AMFComponent* pThis, AMFDataAllocatorCB* callback);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetCaps)(AMFComponent* pThis, AMFCaps** ppCaps);
    AMF_RESULT  (S2P_AMF_STD_CALL *Optimize)(AMFComponent* pThis, AMFComponentOptimizationCallback* pCallback);
};
struct AMFComponent { const AMFComponentVtbl* pVtbl; };

struct AMFContextVtbl {
    amf_long    (S2P_AMF_STD_CALL *Acquire)(AMFContext* pThis);
    amf_long    (S2P_AMF_STD_CALL *Release)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *QueryInterface)(AMFContext* pThis, const AMFGuid* interfaceID, void** ppInterface);
    AMF_RESULT  (S2P_AMF_STD_CALL *SetProperty)(AMFContext* pThis, const wchar_t* name, AMFVariantStruct value);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetProperty)(AMFContext* pThis, const wchar_t* name, AMFVariantStruct* pValue);
    amf_bool    (S2P_AMF_STD_CALL *HasProperty)(AMFContext* pThis, const wchar_t* name);
    amf_size    (S2P_AMF_STD_CALL *GetPropertyCount)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetPropertyAt)(AMFContext* pThis, amf_size index, wchar_t* name, amf_size nameSize, AMFVariantStruct* pValue);
    AMF_RESULT  (S2P_AMF_STD_CALL *Clear)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *AddTo)(AMFContext* pThis, AMFPropertyStorage* pDest, amf_bool overwrite, amf_bool deep);
    AMF_RESULT  (S2P_AMF_STD_CALL *CopyTo)(AMFContext* pThis, AMFPropertyStorage* pDest, amf_bool deep);
    void        (S2P_AMF_STD_CALL *AddObserver)(AMFContext* pThis, AMFPropertyStorageObserver* pObserver);
    void        (S2P_AMF_STD_CALL *RemoveObserver)(AMFContext* pThis, AMFPropertyStorageObserver* pObserver);
    AMF_RESULT  (S2P_AMF_STD_CALL *Terminate)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitDX9)(AMFContext* pThis, void* pDX9Device);
    void*       (S2P_AMF_STD_CALL *GetDX9Device)(AMFContext* pThis, AMF_DX_VERSION dxVersionRequired);
    AMF_RESULT  (S2P_AMF_STD_CALL *LockDX9)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *UnlockDX9)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitDX11)(AMFContext* pThis, void* pDX11Device, AMF_DX_VERSION dxVersionRequired);
    void*       (S2P_AMF_STD_CALL *GetDX11Device)(AMFContext* pThis, AMF_DX_VERSION dxVersionRequired);
    AMF_RESULT  (S2P_AMF_STD_CALL *LockDX11)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *UnlockDX11)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitOpenCL)(AMFContext* pThis, void* pCommandQueue);
    void*       (S2P_AMF_STD_CALL *GetOpenCLContext)(AMFContext* pThis);
    void*       (S2P_AMF_STD_CALL *GetOpenCLCommandQueue)(AMFContext* pThis);
    void*       (S2P_AMF_STD_CALL *GetOpenCLDeviceID)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *GetOpenCLComputeFactory)(AMFContext* pThis, AMFComputeFactory** ppFactory);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitOpenCLEx)(AMFContext* pThis, AMFComputeDevice* pDevice);
    AMF_RESULT  (S2P_AMF_STD_CALL *LockOpenCL)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *UnlockOpenCL)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitOpenGL)(AMFContext* pThis, amf_handle hOpenGLContext, amf_handle hWindow, amf_handle hDC);
    amf_handle  (S2P_AMF_STD_CALL *GetOpenGLContext)(AMFContext* pThis);
    amf_handle  (S2P_AMF_STD_CALL *GetOpenGLDrawable)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *LockOpenGL)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *UnlockOpenGL)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitXV)(AMFContext* pThis, void* pXVDevice);
    void*       (S2P_AMF_STD_CALL *GetXVDevice)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *LockXV)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *UnlockXV)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *InitGralloc)(AMFContext* pThis, void* pGrallocDevice);
    void*       (S2P_AMF_STD_CALL *GetGrallocDevice)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *LockGralloc)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *UnlockGralloc)(AMFContext* pThis);
    AMF_RESULT  (S2P_AMF_STD_CALL *AllocBuffer)(AMFContext* pThis, AMF_MEMORY_TYPE type, amf_size size, AMFBuffer** ppBuffer);
    AMF_RESULT  (S2P_AMF_STD_CALL *AllocSurface)(AMFContext* pThis, AMF_MEMORY_TYPE type, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, AMFSurface** ppSurface);
    AMF_RESULT  (S2P_AMF_STD_CALL *AllocAudioBuffer)(AMFContext* pThis, AMF_MEMORY_TYPE type, int format, amf_int32 samples, amf_int32 sampleRate, amf_int32 channels, AMFAudioBuffer** ppAudioBuffer);
    AMF_RESULT  (S2P_AMF_STD_CALL *CreateBufferFromHostNative)(AMFContext* pThis, void* pHostBuffer, amf_size size, AMFBuffer** ppBuffer, AMFBufferObserver* pObserver);
    AMF_RESULT  (S2P_AMF_STD_CALL *CreateSurfaceFromHostNative)(AMFContext* pThis, AMF_SURFACE_FORMAT format, amf_int32 width, amf_int32 height, amf_int32 hPitch, amf_int32 vPitch, void* pData, AMFSurface** ppSurface, AMFSurfaceObserver* pObserver);
    AMF_RESULT  (S2P_AMF_STD_CALL *CreateSurfaceFromDX9Native)(AMFContext* pThis, void* pDX9Surface, AMFSurface** ppSurface, AMFSurfaceObserver* pObserver);
    AMF_RESULT  (S2P_AMF_STD_CALL *CreateSurfaceFromDX11Native)(AMFContext* pThis, void* pDX11Surface, AMFSurface** ppSurface, AMFSurfaceObserver* pObserver);
};
struct AMFContext { const AMFContextVtbl* pVtbl; };

struct AMFFactoryVtbl {
    AMF_RESULT     (S2P_AMF_STD_CALL *CreateContext)(AMFFactory* pThis, AMFContext** ppContext);
    AMF_RESULT     (S2P_AMF_STD_CALL *CreateComponent)(AMFFactory* pThis, AMFContext* pContext, const wchar_t* id, AMFComponent** ppComponent);
    AMF_RESULT     (S2P_AMF_STD_CALL *SetCacheFolder)(AMFFactory* pThis, const wchar_t* path);
    const wchar_t* (S2P_AMF_STD_CALL *GetCacheFolder)(AMFFactory* pThis);
    AMF_RESULT     (S2P_AMF_STD_CALL *GetDebug)(AMFFactory* pThis, AMFDebug** ppDebug);
    AMF_RESULT     (S2P_AMF_STD_CALL *GetTrace)(AMFFactory* pThis, AMFTrace** ppTrace);
    AMF_RESULT     (S2P_AMF_STD_CALL *GetPrograms)(AMFFactory* pThis, AMFPrograms** ppPrograms);
};
struct AMFFactory { const AMFFactoryVtbl* pVtbl; };

using AMFInit_Fn         = AMF_RESULT(S2P_AMF_CDECL_CALL*)(amf_uint64 version, AMFFactory** ppFactory);
using AMFQueryVersion_Fn = AMF_RESULT(S2P_AMF_CDECL_CALL*)(amf_uint64* pVersion);

// Variant helpers
inline AMFVariantStruct MakeBool(bool v) {
    AMFVariantStruct out{};
    out.type = AMF_VARIANT_BOOL;
    out.boolValue = v;
    return out;
}
inline AMFVariantStruct MakeInt64(amf_int64 v) {
    AMFVariantStruct out{};
    out.type = AMF_VARIANT_INT64;
    out.int64Value = v;
    return out;
}
inline AMFVariantStruct MakeSize(amf_int32 w, amf_int32 h) {
    AMFVariantStruct out{};
    out.type = AMF_VARIANT_SIZE;
    out.sizeValue = AMFSize{ w, h };
    return out;
}
inline AMFVariantStruct MakeRate(amf_uint32 num, amf_uint32 den) {
    AMFVariantStruct out{};
    out.type = AMF_VARIANT_RATE;
    out.rateValue = AMFRate{ num, den };
    return out;
}

inline void ClearVariant(AMFVariantStruct& v) {
    if (v.type == AMF_VARIANT_STRING && v.stringValue) {
        CoTaskMemFree(v.stringValue);
        v.stringValue = nullptr;
    } else if (v.type == AMF_VARIANT_WSTRING && v.wstringValue) {
        CoTaskMemFree(v.wstringValue);
        v.wstringValue = nullptr;
    } else if (v.type == AMF_VARIANT_INTERFACE && v.pInterface) {
        v.pInterface->pVtbl->Release(v.pInterface);
        v.pInterface = nullptr;
    }
    v.type = AMF_VARIANT_EMPTY;
}

// Component IDs & Property Keys (verified against VideoEncoderVCE.h & VideoEncoderHEVC.h)
inline constexpr const wchar_t* kCompAvc  = L"AMFVideoEncoderVCE_AVC";
inline constexpr const wchar_t* kCompHevc = L"AMFVideoEncoderHW_HEVC";

// AVC enums & keys
inline constexpr amf_int64 kAvcUsageUltraLowLatency = 1;
inline constexpr amf_int64 kAvcProfileBaseline = 66;
inline constexpr amf_int64 kAvcProfileMain = 77;
inline constexpr amf_int64 kAvcProfileConstrainedBaseline = 256;
inline constexpr amf_int64 kAvcQualitySpeed = 1;
inline constexpr amf_int64 kAvcRcCbr = 1;
inline constexpr amf_int64 kAvcRcLatencyConstrainedVbr = 3;
inline constexpr amf_int64 kAvcCodingCalv = 2;
inline constexpr amf_int64 kAvcPicNone = 0;
inline constexpr amf_int64 kAvcPicIdr = 2;
inline constexpr amf_int64 kAvcOutIdr = 0;
inline constexpr amf_int64 kAvcOutI = 1;

// HEVC enums & keys
inline constexpr amf_int64 kHevcUsageUltraLowLatency = 1;
inline constexpr amf_int64 kHevcProfileMain = 1;
inline constexpr amf_int64 kHevcQualitySpeed = 10;
inline constexpr amf_int64 kHevcRcLatencyConstrainedVbr = 1;
inline constexpr amf_int64 kHevcRcCbr = 3;
inline constexpr amf_int64 kHevcHdrInsertIdrAligned = 2;
inline constexpr amf_int64 kHevcPicNone = 0;
inline constexpr amf_int64 kHevcPicIdr = 2;
inline constexpr amf_int64 kHevcOutIdr = 0;
inline constexpr amf_int64 kHevcOutI = 1;

} // namespace s2p::amf_abi
