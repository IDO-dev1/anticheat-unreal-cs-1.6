cmake_minimum_required(VERSION 3.16)
project(live_unreal_scanner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

option(BUILD_METAMOD_PLUGIN "Build HL1 Metamod plugin" ON)

add_library(liveac_core STATIC src/detector.cpp)
target_include_directories(liveac_core PUBLIC include)
set_target_properties(liveac_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(liveac_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_METAMOD_PLUGIN)
  if(NOT DEFINED HLSDK_ROOT OR NOT DEFINED METAMOD_ROOT)
    message(FATAL_ERROR "Set -DHLSDK_ROOT=/path/hlsdk -DMETAMOD_ROOT=/path/metamod-hl1")
  endif()

  # HLDS Linux is 32-bit. Every object linked into the module must also be 32-bit.
  target_compile_options(liveac_core PRIVATE -m32)

  add_library(live_unreal_scanner SHARED src/plugin.cpp)
  target_include_directories(live_unreal_scanner PRIVATE
    include
    ${HLSDK_ROOT}/common
    ${HLSDK_ROOT}/dlls
    ${HLSDK_ROOT}/engine
    ${HLSDK_ROOT}/pm_shared
    ${HLSDK_ROOT}/public
    ${METAMOD_ROOT}/metamod
  )
  target_compile_definitions(live_unreal_scanner PRIVATE
    _LINUX linux
    stricmp=strcasecmp
    strnicmp=strncasecmp
  )
  target_compile_options(live_unreal_scanner PRIVATE
    -m32 -Wall -Wextra -Wno-unused-parameter
  )
  target_link_options(live_unreal_scanner PRIVATE
    -m32 -static-libgcc -static-libstdc++
  )
  target_link_libraries(live_unreal_scanner PRIVATE liveac_core)
  set_target_properties(live_unreal_scanner PROPERTIES
    PREFIX ""
    OUTPUT_NAME "live_unreal_scanner_mm_i386"
  )
endif()

add_executable(detector_test tests/test_detector.cpp)
target_link_libraries(detector_test PRIVATE liveac_core)
if(BUILD_METAMOD_PLUGIN)
  target_compile_options(detector_test PRIVATE -m32)
  target_link_options(detector_test PRIVATE -m32)
endif()

enable_testing()
add_test(NAME detector_test COMMAND detector_test)
