cmake_minimum_required(VERSION 3.10)
project(SteamFrameDenoise CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include_directories(include)
enable_testing()

add_executable(test_pipeline tests/test_pipeline.cpp)
add_test(NAME pipeline COMMAND test_pipeline)

# scripts/steamframe-passthrough-ctl edits config.ini and reads the layer's log; needs only bash.
find_program(SF_BASH bash)
if(SF_BASH)
  add_test(NAME ctl COMMAND ${SF_BASH} ${CMAKE_SOURCE_DIR}/tests/ctl_test.sh
           ${CMAKE_SOURCE_DIR}/scripts/steamframe-passthrough-ctl)
endif()

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

# The manifest an installed layer is registered with (get.sh writes the same JSON; keep them in sync).
# It is an implicit layer, so the loader enables it for every OpenXR app by default. There is
# deliberately no "enable_environment": the loader silently skips an implicit layer that declares one
# unless that variable is set in the app's environment, so an installed layer would do nothing on a
# headset where apps are started from the dashboard (reproduced by layer_smoke_implicit below).
# "disable_environment" is required for implicit layers and is the per-app opt-out.
set(SF_IMPLICIT_MANIFEST [=[{
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_STEAMFRAME_passthrough_denoise",
    "library_path": "@SF_LIB@",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "Temporal denoise + CAS for monochrome passthrough",
    "functions": {
      "xrNegotiateLoaderApiLayerInterface": "xrNegotiateLoaderApiLayerInterface"
    },
    "disable_environment": "DISABLE_XR_APILAYER_STEAMFRAME_passthrough_denoise"
  }
}
]=])

# `make install` mirrors get.sh: user-local, no root needed (SteamOS-safe).
# Override with -DSF_INSTALL_HOME=<dir>; defaults to $HOME.
if(TARGET steamframe_layer)
  if(DEFINED ENV{HOME})
    set(_sf_home "$ENV{HOME}")
  endif()
  if(SF_INSTALL_HOME)
    set(_sf_home "${SF_INSTALL_HOME}")
  endif()
  set(SF_LIB_DIR "${_sf_home}/.local/lib/openxr")
  install(TARGETS steamframe_layer LIBRARY DESTINATION "${SF_LIB_DIR}")
  # Written to a subdirectory, not CMAKE_BINARY_DIR directly: this manifest declares the same
  # layer "name" as layer/XR_APILAYER_SteamFrame_passthrough_denoise.json (configure_file'd right
  # into CMAKE_BINARY_DIR above for dev use with XR_API_LAYER_PATH=<build dir>, per SETUP.md). Two
  # manifests for the same layer name in one XR_API_LAYER_PATH directory is exactly the kind of
  # thing that looks harmless (one has a dangling library_path pointing at an not-yet-installed
  # $HOME location) but isn't: at least one real loader (Ubuntu 24.04's libopenxr-loader1 1.0.20)
  # refuses to load the layer at all when it sees a duplicate name, even though the other manifest
  # for it is perfectly valid -- confirmed by this exact collision breaking the layer_smoke ctest
  # on Ubuntu while passing on a newer loader (Arch's openxr 1.1.60) that's lenient about it.
  set(SF_INSTALL_MANIFEST ${CMAKE_BINARY_DIR}/install-staging/steamframe_passthrough.json)
  string(REPLACE "@SF_LIB@" "${SF_LIB_DIR}/libsteamframe_layer.so" _sf_manifest "${SF_IMPLICIT_MANIFEST}")
  file(WRITE ${SF_INSTALL_MANIFEST} "${_sf_manifest}")
  install(FILES ${SF_INSTALL_MANIFEST}
          DESTINATION "${_sf_home}/.local/share/openxr/1/api_layers/implicit.d")
  # Launchers for the SteamVR dashboard's "+" list, same as get.sh installs: a small script plus two
  # .desktop entries that run it in Konsole. configure_file fills in the script's absolute path
  # (Steam may start launchers without ~/.local/bin in PATH).
  install(PROGRAMS scripts/steamframe-passthrough-ctl DESTINATION "${_sf_home}/.local/bin")
  set(CTL "${_sf_home}/.local/bin/steamframe-passthrough-ctl")
  foreach(_d steamframe-passthrough-toggle steamframe-passthrough-status)
    configure_file(contrib/${_d}.desktop ${CMAKE_BINARY_DIR}/install-staging/${_d}.desktop @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/install-staging/${_d}.desktop
            DESTINATION "${_sf_home}/.local/share/applications")
  endforeach()
  # Keep an existing user config untouched.
  install(CODE "
    set(_cfg \"${_sf_home}/.config/steamframe-passthrough/config.ini\")
    if(NOT EXISTS \"\${_cfg}\")
      file(INSTALL \"${CMAKE_SOURCE_DIR}/config.ini\" DESTINATION \"${_sf_home}/.config/steamframe-passthrough\")
    endif()")
endif()

# Integration smoke test: a fake OpenXR runtime (tests/integration/stub_runtime.cpp) plus a
# Vulkan+OpenXR test app (tests/integration/smoke_test.cpp) that drives the real layer end-to-end
# with no HMD/compositor needed. Requires the actual loader library to link the test app against
# (optional for the layer itself -- see the comment above -- but required here). Not installed;
# dev/test tooling only. See tests/integration/README.md.
if(TARGET steamframe_layer AND OPENXR_LOADER)
  add_library(sf_stub_runtime SHARED tests/integration/stub_runtime.cpp)
  target_include_directories(sf_stub_runtime PRIVATE ${OPENXR_INCLUDE_DIR})
  target_link_libraries(sf_stub_runtime PRIVATE Vulkan::Vulkan)
  set_target_properties(sf_stub_runtime PROPERTIES PREFIX "lib")

  add_executable(layer_smoke_test tests/integration/smoke_test.cpp)
  target_include_directories(layer_smoke_test PRIVATE ${OPENXR_INCLUDE_DIR})
  target_link_libraries(layer_smoke_test PRIVATE Vulkan::Vulkan ${OPENXR_LOADER})

  # file(GENERATE) supports $<TARGET_FILE:...>, so the manifest can embed the real build path to
  # the stub runtime .so without a separate post-build step.
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/integration/active_runtime.json CONTENT
"{
  \"file_format_version\": \"1.0.0\",
  \"runtime\": {
    \"library_path\": \"$<TARGET_FILE:sf_stub_runtime>\",
    \"name\": \"SteamFrame Stub Test Runtime\"
  }
}
")

  # Explicit activation, as a developer uses it (SETUP.md): XR_API_LAYER_PATH + XR_ENABLE_API_LAYERS.
  add_test(NAME layer_smoke COMMAND layer_smoke_test)
  set_tests_properties(layer_smoke PROPERTIES
    ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/integration/active_runtime.json;XR_API_LAYER_PATH=${CMAKE_BINARY_DIR};XR_ENABLE_API_LAYERS=XR_APILAYER_STEAMFRAME_passthrough_denoise;SF_CONFIG=${CMAKE_BINARY_DIR}/integration/smoke_config.ini;SF_LOG=${CMAKE_BINARY_DIR}/integration/layer.log"
    SKIP_RETURN_CODE 77)

  # Installed activation, as a user gets it from get.sh / `cmake --install`: the same implicit
  # manifest, pointing at the freshly built library, in the XDG location the loader scans -- and no
  # XR_API_LAYER_PATH, XR_ENABLE_API_LAYERS or ENABLE_* variable at all. This is the test that would
  # have caught the installed layer being silently skipped by the loader.
  string(REPLACE "@SF_LIB@" "$<TARGET_FILE:steamframe_layer>" _sf_manifest_test "${SF_IMPLICIT_MANIFEST}")
  file(GENERATE
       OUTPUT ${CMAKE_BINARY_DIR}/integration/xdg/openxr/1/api_layers/implicit.d/steamframe_passthrough.json
       CONTENT "${_sf_manifest_test}")
  add_test(NAME layer_smoke_implicit COMMAND layer_smoke_test)
  set_tests_properties(layer_smoke_implicit PROPERTIES
    ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_BINARY_DIR}/integration/active_runtime.json;XDG_DATA_HOME=${CMAKE_BINARY_DIR}/integration/xdg;SF_CONFIG=${CMAKE_BINARY_DIR}/integration/smoke_config_implicit.ini;SF_LOG=${CMAKE_BINARY_DIR}/integration/layer_implicit.log"
    SKIP_RETURN_CODE 77)
else()
  message(STATUS "OpenXR loader library not found: skipping layer_smoke integration test")
endif()
