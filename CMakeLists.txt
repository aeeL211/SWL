cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  shadowhook
  GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
  GIT_TAG        v1.0.10
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  shadowhook
)
FetchContent_MakeAvailable(shadowhook)

add_library(fbrian SHARED main.cpp)

target_include_directories(fbrian PRIVATE
  ${shadowhook_SOURCE_DIR}/include
)

target_link_libraries(fbrian PRIVATE
  shadowhook
  log
  android
  dl
)

set_target_properties(fbrian PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "fbrian"
)
