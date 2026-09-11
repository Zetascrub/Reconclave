# The root project() only declares LANGUAGES CXX; third_party/lvgl_drm_rotate/
# lv_linux_drm.c (below, in the UI target) is plain C, so CMake needs this to
# even attempt compiling it - without it, add_executable() silently drops
# unrecognized-language sources rather than erroring, which is exactly what
# happened here once (looked like a successful build/link, only failed at
# actual runtime symbol resolution - see devices/k230/README.md's UI section).
enable_language(C)

add_executable(reconclave_k230
  main.cpp
  src/json.cpp
  src/http_server.cpp
  src/mdns_responder.cpp
  src/wifi_scanner.cpp
  src/network_inventory.cpp
  src/gnss_receiver.cpp
  src/network_tools.cpp
  src/trust_crypto.cpp
  src/trust_policy.cpp
  src/discovery_job.cpp
  src/evidence_store.cpp
  src/host_knowledge.cpp
)

target_link_libraries(reconclave_k230 PRIVATE reconclave_protocol pthread)
target_include_directories(reconclave_k230 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Standalone hardware-verification tool for the TCA8418 keyboard driver -
# not part of the Reconclave app/protocol surface, see kbtest.cpp.
add_executable(reconclave_k230_kbtest
  kbtest.cpp
  src/tca8418_keyboard.cpp
)
target_include_directories(reconclave_k230_kbtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_k230_kbtest PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Standalone hardware-verification tool for the Wi-Fi scanner - not part of
# the Reconclave app/protocol surface, see wifitest.cpp.
add_executable(reconclave_k230_wifitest
  wifitest.cpp
  src/wifi_scanner.cpp
)
target_include_directories(reconclave_k230_wifitest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_k230_wifitest PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# UI milestone 1 proof (DRM + LVGL + touch) - links against the device's
# actual liblvgl/liblvgl_linux/libdrm rather than a self-built LVGL, see
# devices/k230/fetch-device-sysroot.sh and the README's UI section. Not
# built unless that sysroot has actually been fetched, since it needs
# real device-pulled .so files this repo does not (and should not) commit.
if(NOT DEFINED K230_DEVICE_SYSROOT)
  set(K230_DEVICE_SYSROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.toolchains/device-sysroot")
endif()
if(EXISTS "${K230_DEVICE_SYSROOT}/lib/liblvgl.so")
  add_executable(reconclave_k230_drm_proof
    drm_proof.cpp
  )
  target_include_directories(reconclave_k230_drm_proof PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${K230_DEVICE_SYSROOT}/include"
    "${K230_DEVICE_SYSROOT}/include/lvgl"
    "${K230_DEVICE_SYSROOT}/include/drm"
  )
  target_link_directories(reconclave_k230_drm_proof PRIVATE "${K230_DEVICE_SYSROOT}/lib")
  # The SDK installs lv_conf.h alongside lvgl.h under include/lvgl.  Define
  # this target-wide (rather than in one source file) so every translation
  # unit, including headers that include LVGL first, uses that layout.
  target_compile_definitions(reconclave_k230_drm_proof PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
  target_link_libraries(reconclave_k230_drm_proof PRIVATE lvgl lvgl_linux drm)

  # The touch UI itself (§18 mockup) - see devices/k230/README.md.
  # lv_linux_drm_set_rotation() is declared in
  # third_party/lvgl_drm_rotate/lv_linux_drm.h (kept for the declaration
  # only - see that header's comment) but NOT compiled from
  # lv_linux_drm.c here: Canaan/LILYGO's own liblvgl.so (not
  # liblvgl_linux.so - the core library) already exports a natively-built,
  # working lv_linux_drm_set_rotation (confirmed via `nm -D`, and it's what
  # k230_phone_ui itself calls), so this links against their proven
  # implementation instead of a from-scratch one written against upstream
  # LVGL, which after several iterations still had an unresolved
  # pixel-mapping bug.
  add_executable(reconclave_k230_ui
    ui_app.cpp
    src/ui_shell.cpp
    src/wifi_scanner.cpp
    src/network_inventory.cpp
    src/gnss_receiver.cpp
    src/json.cpp
    src/trust_crypto.cpp
    src/evidence_store.cpp
    src/host_knowledge.cpp
    src/camera_capture.cpp
    assets/zeta_k230_logo.c
  )
  target_include_directories(reconclave_k230_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${K230_DEVICE_SYSROOT}/include"
    "${K230_DEVICE_SYSROOT}/include/lvgl"
    "${K230_DEVICE_SYSROOT}/include/drm"
  )
  target_link_directories(reconclave_k230_ui PRIVATE "${K230_DEVICE_SYSROOT}/lib")
  target_compile_definitions(reconclave_k230_ui PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
  # pthread: the VISION screen's live preview runs its own capture loop on
  # a std::thread rather than blocking the LVGL/UI thread for each ~1s
  # camera capture (that blocking approach was tried first and made the
  # whole UI visibly unresponsive - see devices/k230/README.md). atomic:
  # this toolchain emits a libatomic call (__atomic_exchange_1) for
  # std::atomic<bool>::exchange() even with -march=rv64gc's "A" extension
  # enabled - confirmed by the link failing without this.
  target_link_libraries(reconclave_k230_ui PRIVATE lvgl lvgl_linux drm pthread atomic)
else()
  message(STATUS "K230 UI targets skipped: run devices/k230/fetch-device-sysroot.sh first")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(reconclave_k230 PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
