// Minimal copy of the OpenXR loader <-> runtime negotiation structs (the public openxr-dev
// packages do not ship loader_interfaces.h). Used only by stub_runtime.cpp; this is the runtime
// side of the same negotiation include/loader_interfaces.h implements for the layer side.
#pragma once
#include <openxr/openxr.h>

#define XR_CURRENT_LOADER_RUNTIME_VERSION 1

typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
} XrLoaderInterfaceStructs;

typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateRuntimeRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t runtimeInterfaceVersion;
    XrVersion runtimeApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
} XrNegotiateRuntimeRequest;

typedef XrResult(XRAPI_PTR* PFN_xrNegotiateLoaderRuntimeInterface)(const XrNegotiateLoaderInfo*,
                                                                    XrNegotiateRuntimeRequest*);
