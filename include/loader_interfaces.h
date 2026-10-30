// Minimal copy of the OpenXR loader <-> API layer negotiation structs
// (the public openxr-dev packages do not ship loader_interfaces.h).
#pragma once
#include <openxr/openxr.h>

#define XR_LOADER_INFO_STRUCT_VERSION 1
#define XR_API_LAYER_INFO_STRUCT_VERSION 1
#define XR_API_LAYER_NEXT_INFO_STRUCT_VERSION 1
#define XR_CURRENT_LOADER_API_LAYER_VERSION 1
#define XR_API_LAYER_MAX_SETTINGS_PATH_SIZE 512

typedef enum XrLoaderInterfaceStructs {
    XR_LOADER_INTERFACE_STRUCT_UNINTIALIZED = 0,
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST = 3,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO = 4,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO = 5,
} XrLoaderInterfaceStructs;

struct XrApiLayerCreateInfo;
typedef XrResult(XRAPI_PTR* PFN_xrCreateApiLayerInstance)(const XrInstanceCreateInfo* info,
                                                          const struct XrApiLayerCreateInfo* layerInfo,
                                                          XrInstance* instance);

typedef struct XrNegotiateLoaderInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct XrNegotiateApiLayerRequest {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    PFN_xrCreateApiLayerInstance createApiLayerInstance;
} XrNegotiateApiLayerRequest;

typedef struct XrApiLayerNextInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    char layerName[XR_MAX_API_LAYER_NAME_SIZE];
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance;
    struct XrApiLayerNextInfo* next;
} XrApiLayerNextInfo;

typedef struct XrApiLayerCreateInfo {
    XrLoaderInterfaceStructs structType;
    uint32_t structVersion;
    size_t structSize;
    void* loaderInstance;
    char settings_file_location[XR_API_LAYER_MAX_SETTINGS_PATH_SIZE];
    XrApiLayerNextInfo* nextInfo;
} XrApiLayerCreateInfo;

typedef XrResult(XRAPI_PTR* PFN_xrNegotiateLoaderApiLayerInterface)(const XrNegotiateLoaderInfo*, const char*,
                                                                    XrNegotiateApiLayerRequest*);
