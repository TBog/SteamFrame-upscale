cmake_minimum_required(VERSION 3.10)
project(SteamFrameDenoise CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include_directories(include)
enable_testing()

add_executable(test_pipeline tests/test_pipeline.cpp)
add_test(NAME pipeline COMMAND test_pipeline)

# OpenXR API layer (needs Vulkan + OpenXR dev packages and glslangValidator:
#   apt install libvulkan-dev libopenxr-dev glslang-tools)
find_package(Vulkan)
find_path(OPENXR_INCLUDE_DIR openxr/openxr.h)
find_library(OPENXR_LOADER openxr_loader)
find_program(GLSLANG glslangValidator)
if(Vulkan_FOUND AND OPENXR_INCLUDE_DIR AND GLSLANG)
  set(SPV_HEADER ${CMAKE_BINARY_DIR}/generated/denoise_spv.h)
  add_custom_command(
    OUTPUT ${SPV_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${GLSLANG} -V --variable-name denoise_cas_spv -o ${SPV_HEADER} ${CMAKE_SOURCE_DIR}/shaders/denoise_cas.comp
    DEPENDS shaders/denoise_cas.comp)
  add_library(steamframe_layer SHARED src/layer.cpp ${SPV_HEADER})
  target_include_directories(steamframe_layer PRIVATE ${OPENXR_INCLUDE_DIR} ${CMAKE_BINARY_DIR}/generated)
  target_link_libraries(steamframe_layer PRIVATE Vulkan::Vulkan)
  if(OPENXR_LOADER)  # the layer itself only needs the headers; linking the loader is optional
    target_link_libraries(steamframe_layer PRIVATE ${OPENXR_LOADER})
  endif()
  set_target_properties(steamframe_layer PROPERTIES CXX_VISIBILITY_PRESET hidden PREFIX "lib")
  configure_file(layer/XR_APILAYER_SteamFrame_passthrough_denoise.json
                 ${CMAKE_BINARY_DIR}/XR_APILAYER_SteamFrame_passthrough_denoise.json COPYONLY)
else()
  message(STATUS "Vulkan/OpenXR/glslang not found: skipping steamframe_layer")
endif()
