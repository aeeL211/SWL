cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
