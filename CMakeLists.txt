cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ShadowHook: path diberikan oleh build.yml (hasil ekstrak AAR)
#   -DSHADOWHOOK_INCLUDE_DIR=<folder berisi shadowhook.h>
#   -DSHADOWHOOK_LIB=<path libshadowhook.so untuk ABI ini>
if(NOT SHADOWHOOK_INCLUDE_DIR OR NOT SHADOWHOOK_LIB)
  message(FATAL_ERROR "Set SHADOWHOOK_INCLUDE_DIR dan SHADOWHOOK_LIB")
endif()

add_library(shadowhook SHARED IMPORTED)
set_target_properties(shadowhook PROPERTIES
  IMPORTED_LOCATION "${SHADOWHOOK_LIB}"
  IMPORTED_SONAME "libshadowhook.so"
)

add_library(fbrian SHARED
  main.cpp
)

target_include_directories(fbrian PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SHADOWHOOK_INCLUDE_DIR}
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
  -Wl,-z,max-page-size=16384
)

# dl (dlopen/dlsym) dan shadowhook. liblog tidak lagi dibutuhkan (logging dihapus).
target_link_libraries(fbrian PRIVATE dl shadowhook)

set_target_properties(fbrian PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "fbrian"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib/${ANDROID_ABI}"
)
