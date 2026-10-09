cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  shadowhook
  GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
  GIT_TAG        v2.0.1
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  shadowhook/src/main/cpp
  PATCH_COMMAND  sed -i "s/cmake_minimum_required(VERSION 4.0.2)/cmake_minimum_required(VERSION 3.20)/" shadowhook/src/main/cpp/CMakeLists.txt
)
FetchContent_MakeAvailable(shadowhook)

target_compile_options(shadowhook PRIVATE -Wno-error)
target_compile_options(shadowhook PRIVATE -Wno-declaration-after-statement)
target_compile_options(shadowhook PRIVATE -Wno-unsafe-buffer-usage)

target_link_options(shadowhook PRIVATE -Wl,--undefined-version)

add_library(fbrian SHARED main.cpp)

target_include_directories(fbrian PRIVATE
  ${shadowhook_SOURCE_DIR}/shadowhook/src/main/cpp/include
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
