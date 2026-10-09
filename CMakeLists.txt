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

add_library(fbrian SHARED 
  main.cpp
  And64InlineHook.cpp
)

target_include_directories(fbrian PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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
)

target_link_libraries(fbrian PRIVATE dl)

set_target_properties(fbrian PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "fbrian"
)
