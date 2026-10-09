cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(
  and64inlinehook
  GIT_REPOSITORY https://github.com/Rprop/And64InlineHook.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(and64inlinehook)

add_library(fbrian SHARED main.cpp)

target_include_directories(fbrian PRIVATE
  ${and64inlinehook_SOURCE_DIR}
)

target_sources(fbrian PRIVATE
  ${and64inlinehook_SOURCE_DIR}/And64InlineHook.cpp
)

target_compile_options(fbrian PRIVATE
  -Os
  -fno-exceptions
  -fno-rtti
  -ffunction-sections
  -fdata-sections
  -fvisibility=hidden
  -fvisibility-inlines-hidden
)

target_link_options(fbrian PRIVATE
  -Wl,--gc-sections
  -Wl,--strip-all
  -Wl,--exclude-libs,ALL
)

target_link_libraries(fbrian PRIVATE
  log
  android
  dl
)

set_target_properties(fbrian PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "fbrian"
)
