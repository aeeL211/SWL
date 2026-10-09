cmake_minimum_required(VERSION 3.20)
project(SWLMods CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  dobby
  GIT_REPOSITORY https://github.com/jmpews/Dobby.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
  PATCH_COMMAND  sed -i
    -e "s/common_closure_bridge_handler@PAGEOFF/:lo12:common_closure_bridge_handler/"
    -e "s/common_closure_bridge_handler@PAGE/common_closure_bridge_handler/"
    source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm
)
set(DOBBY_DEBUG           OFF CACHE BOOL "" FORCE)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(dobby)

add_library(fbrian SHARED main.cpp)

target_include_directories(fbrian PRIVATE ${dobby_SOURCE_DIR}/include)

target_link_libraries(fbrian PRIVATE dobby_static log android dl)

set_target_properties(fbrian PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "fbrian"
)
