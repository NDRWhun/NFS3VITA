# Compiles Cg shaders with the SCE shader compiler and embeds the .gxp output as C arrays.

if(NOT DEFINED SCE_PSP2_SDK_DIR)
  if(DEFINED ENV{SCE_PSP2_SDK_DIR})
    set(SCE_PSP2_SDK_DIR "$ENV{SCE_PSP2_SDK_DIR}" CACHE PATH "Official PSP2 SDK root (needs host_tools/bin/psp2cgc.exe)")
  else()
    message(FATAL_ERROR "Set SCE_PSP2_SDK_DIR (or the SCE_PSP2_SDK_DIR environment variable) to the official SDK root.")
  endif()
endif()

find_program(PSP2CGC psp2cgc
  PATHS "${SCE_PSP2_SDK_DIR}/host_tools/bin"
  NO_DEFAULT_PATH)
if(NOT PSP2CGC)
  message(FATAL_ERROR "psp2cgc not found under ${SCE_PSP2_SDK_DIR}/host_tools/bin")
endif()

set(NFS3_SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${NFS3_SHADER_BINARY_DIR}")

# nfs3_add_shader(<list-var> <name> <sce_vp_psp2|sce_fp_psp2>)
# Adds shaders/<name>.cg -> <name>.gxp -> <name>_gxp.c (symbols nfs3_shader_<name>, nfs3_shader_<name>_size).
function(nfs3_add_shader out_var name profile)
  set(src "${CMAKE_SOURCE_DIR}/shaders/${name}.cg")
  set(gxp "${NFS3_SHADER_BINARY_DIR}/${name}.gxp")
  set(csrc "${NFS3_SHADER_BINARY_DIR}/${name}_gxp.c")
  add_custom_command(
    OUTPUT "${gxp}"
    COMMAND "${PSP2CGC}" -profile ${profile} -O3 -Wperf -o "${gxp}" "${src}"
    DEPENDS "${src}"
    COMMENT "Compiling shader ${name}.cg (${profile})"
    VERBATIM)
  add_custom_command(
    OUTPUT "${csrc}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${gxp}" "-DOUTPUT=${csrc}" "-DSYMBOL=nfs3_shader_${name}"
            -P "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    DEPENDS "${gxp}" "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    COMMENT "Embedding shader ${name}.gxp"
    VERBATIM)
  list(APPEND ${out_var} "${csrc}")
  set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()
