# NFS3VITA - Need for Speed III: Hot Pursuit (static recompilation) -> PS Vita. ELF -> SELF -> VPK.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define the VITASDK environment variable (point at your VitaSDK root).")
  endif()
endif()

project(NFS3VITA C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# GNU mode keeps newlib's POSIX declarations (ftruncate, strcasecmp) visible.
set(CMAKE_CXX_EXTENSIONS ON)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "NFS3VITA")
set(VITA_TITLEID  "NFS3VITA1")
set(VITA_VERSION  "01.00")

option(NFS3_LOG_DEBUG        "Verbose runtime logging" OFF)
option(NFS3_ASSERT_SOFT      "Log failed assertions instead of trapping" OFF)
option(NFS3_DEBUG_INSTALLWIN "Log the parsed install.win table at video mode set" OFF)
option(NFS3_DEBUG_FRAMEDUMP  "Write presented 16-bit frames as raw 565 files to ux0:data/nfs3vita/dump" OFF)
option(NFS3_PROFILE          "Sampling profiler build: instrumented copies of the generated code" OFF)
option(NFS3_RUNTIME_CHECK    "Only compile the runtime sources that build without the generated code (CI)" OFF)

include(cmake/nfs3_shaders.cmake)
include(cmake/nfs3_sources.cmake)

set(NFS3_SHADER_SOURCES "")
nfs3_add_shader(NFS3_SHADER_SOURCES glide_v sce_vp_psp2)
nfs3_add_shader(NFS3_SHADER_SOURCES glide_f sce_fp_psp2)
nfs3_add_shader(NFS3_SHADER_SOURCES blit_v  sce_vp_psp2)
nfs3_add_shader(NFS3_SHADER_SOURCES blit_f  sce_fp_psp2)
nfs3_add_shader(NFS3_SHADER_SOURCES clear_v sce_vp_psp2)
nfs3_add_shader(NFS3_SHADER_SOURCES clear_f sce_fp_psp2)

set(NFS3_COMMON_FLAGS
  -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables
  -ffunction-sections -fdata-sections
  -Wall -Wextra
)
# Generated code only sees the stable defines; debug switches stay in the runtime to avoid full rebuilds.
set(NFS3_DEFINES VITA __vita__ WITH_MMX)
set(NFS3_RUNTIME_DEFINES ${NFS3_DEFINES})
if(NFS3_LOG_DEBUG)
  list(APPEND NFS3_RUNTIME_DEFINES NFS3_LOG_DEBUG)
endif()
if(NFS3_ASSERT_SOFT)
  list(APPEND NFS3_RUNTIME_DEFINES NFS3_ASSERT_SOFT)
endif()
if(NFS3_DEBUG_INSTALLWIN)
  list(APPEND NFS3_RUNTIME_DEFINES NFS3_DEBUG_INSTALLWIN)
endif()
if(NFS3_DEBUG_FRAMEDUMP)
  set(NFS3_DEBUG_FRAMEDUMP_SKIP 0 CACHE STRING "Presents to skip before dumping frames")
  list(APPEND NFS3_RUNTIME_DEFINES NFS3_DEBUG_FRAMEDUMP NFS3_DEBUG_FRAMEDUMP_SKIP=${NFS3_DEBUG_FRAMEDUMP_SKIP})
endif()

set(NFS3_GENERATED_DIR "${CMAKE_SOURCE_DIR}/src/nfs3hp/disassembly" CACHE PATH "Generated game code (tools/generate.ps1)")
set(NFS3_INCLUDES
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/nfs3hp
  ${NFS3_GENERATED_DIR}
)

# Compiles the runtime sources that do not need the generated game code; no VPK (CI without game binaries).
if(NFS3_RUNTIME_CHECK)
  add_library(nfs3_runtime_check OBJECT ${NFS3_STANDALONE_SOURCES} ${NFS3_SHADER_SOURCES})
  target_compile_options(nfs3_runtime_check PRIVATE ${NFS3_COMMON_FLAGS} -O3 -Werror)
  target_compile_definitions(nfs3_runtime_check PRIVATE ${NFS3_RUNTIME_DEFINES})
  target_include_directories(nfs3_runtime_check PRIVATE ${NFS3_INCLUDES})
  return()
endif()

file(GLOB NFS3_DISASM_SOURCES CONFIGURE_DEPENDS "${NFS3_GENERATED_DIR}/*.cpp")
if(NOT NFS3_DISASM_SOURCES)
  message(FATAL_ERROR "${NFS3_GENERATED_DIR} is empty; run tools/generate.ps1 first.")
endif()

# Generated game code: large, optimise for size and compile time.
add_library(nfs3_game OBJECT ${NFS3_DISASM_SOURCES})
target_compile_options(nfs3_game PRIVATE ${NFS3_COMMON_FLAGS} -O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-parameter)
# Start-up method registration (nfs3hp.cpp, runs once) and the software renderer (softtria; the port selects the
# Voodoo2 driver) dominate compile time and gain nothing from -O2: -Og compiles them 4-19x faster.
set(NFS3_LIGHT_SOURCES ${NFS3_DISASM_SOURCES})
list(FILTER NFS3_LIGHT_SOURCES INCLUDE REGEX "/(nfs3hp|softtria\\.[^/]+)\\.cpp$")
set_source_files_properties(${NFS3_LIGHT_SOURCES} PROPERTIES COMPILE_OPTIONS -Og)
target_compile_definitions(nfs3_game PRIVATE ${NFS3_DEFINES})
target_include_directories(nfs3_game PRIVATE ${NFS3_INCLUDES})

# Runtime and Vita backend.
add_library(nfs3_runtime OBJECT ${NFS3_RUNTIME_SOURCES} ${NFS3_SHADER_SOURCES})
target_compile_options(nfs3_runtime PRIVATE ${NFS3_COMMON_FLAGS} -O3)
target_compile_definitions(nfs3_runtime PRIVATE ${NFS3_RUNTIME_DEFINES})
target_include_directories(nfs3_runtime PRIVATE ${NFS3_INCLUDES})

add_executable(NFS3VITA.elf src/nfs3hp/nfs3hp_main.cpp $<TARGET_OBJECTS:nfs3_game> $<TARGET_OBJECTS:nfs3_runtime>)
target_compile_options(NFS3VITA.elf PRIVATE ${NFS3_COMMON_FLAGS} -O2)
target_compile_definitions(NFS3VITA.elf PRIVATE ${NFS3_RUNTIME_DEFINES})
target_include_directories(NFS3VITA.elf PRIVATE ${NFS3_INCLUDES})
target_link_options(NFS3VITA.elf PRIVATE -Wl,--gc-sections)
target_link_libraries(NFS3VITA.elf
  stdc++
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAudio_stub
  ScePower_stub
  SceRtc_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  SceProcessmgr_stub
  c
  m
)

vita_create_self(NFS3VITA.self NFS3VITA.elf UNSAFE)

set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_vpk(NFS3VITA.vpk ${VITA_TITLEID} NFS3VITA.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)

if(NFS3_PROFILE)
  include(cmake/nfs3_profile.cmake)
endif()
