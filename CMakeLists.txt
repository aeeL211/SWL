cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- ABI guard ---------------------------------------------------------------
if(NOT DEFINED ANDROID_ABI OR NOT ANDROID_ABI MATCHES "^(arm64-v8a|armeabi-v7a)$")
  message(FATAL_ERROR
    "Unsupported or missing ANDROID_ABI='${ANDROID_ABI}'. "
    "Use the NDK toolchain file with arm64-v8a or armeabi-v7a.")
endif()

# --- ShadowHook (unzipped AAR from Maven Central) ------------------------------
# Layout inside the AAR (prefab):
#   prefab/modules/shadowhook/include/shadowhook.h
#   prefab/modules/shadowhook/libs/android.<abi>/libshadowhook.so
set(SHADOWHOOK_ROOT "" CACHE PATH "Root directory of the unzipped shadowhook AAR")
if(NOT SHADOWHOOK_ROOT)
  message(FATAL_ERROR "Pass -DSHADOWHOOK_ROOT=<dir of unzipped shadowhook AAR>")
endif()

set(SHADOWHOOK_PREFAB "${SHADOWHOOK_ROOT}/prefab/modules/shadowhook")
set(SHADOWHOOK_INCLUDE_DIR "${SHADOWHOOK_PREFAB}/include")
set(SHADOWHOOK_LIB "${SHADOWHOOK_PREFAB}/libs/android.${ANDROID_ABI}/libshadowhook.so")

if(NOT EXISTS "${SHADOWHOOK_INCLUDE_DIR}/shadowhook.h")
  message(FATAL_ERROR "shadowhook.h not found in ${SHADOWHOOK_INCLUDE_DIR}")
endif()
if(NOT EXISTS "${SHADOWHOOK_LIB}")
  message(FATAL_ERROR "libshadowhook.so not found for ${ANDROID_ABI}: ${SHADOWHOOK_LIB}")
endif()

add_library(shadowhook SHARED IMPORTED)
set_target_properties(shadowhook PROPERTIES
  IMPORTED_LOCATION "${SHADOWHOOK_LIB}"
  IMPORTED_SONAME "libshadowhook.so"
  INTERFACE_INCLUDE_DIRECTORIES "${SHADOWHOOK_INCLUDE_DIR}"
)

# --- Project -------------------------------------------------------------------
# logcat output; OFF gives the smallest .so (-DFB_LOG=OFF)
option(FB_LOG "Enable logcat output" ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(fbrian SHARED 
  main.cpp
)

target_compile_options(fbrian PRIVATE
  -Oz
  -flto
  -fno-exceptions
  -fno-rtti
  -fno-unwind-tables
  -fno-asynchronous-unwind-tables
  -fno-stack-protector
  -fno-ident
  -fomit-frame-pointer
  -ffunction-sections
  -fdata-sections
  -fvisibility=hidden
  -fvisibility-inlines-hidden
)

target_link_options(fbrian PRIVATE
  -flto
  -Wl,--gc-sections
  -Wl,--icf=all
  -Wl,--strip-all
  -Wl,--exclude-libs,ALL
  -Wl,--build-id=none
  -Wl,--hash-style=gnu
  -Wl,--no-rosegment
  -Wl,-z,norelro
  -Wl,--pack-dyn-relocs=android
)

# dl: dlopen/dlsym, shadowhook: inline hook (DT_NEEDED)
target_link_libraries(fbrian PRIVATE shadowhook dl)

if(FB_LOG)
  target_compile_definitions(fbrian PRIVATE FB_LOG)
  target_link_libraries(fbrian PRIVATE log)
endif()

# Output: <build>/lib/<ANDROID_ABI>/libfbrian.so
set_target_properties(fbrian PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "fbrian"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${ANDROID_ABI}"
)
