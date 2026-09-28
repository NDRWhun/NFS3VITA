# Shaders: Cg sources in shaders/, prebuilt .gxp binaries in shaders/gxp/, embedded as C arrays.
# With SCE_PSP2_SDK_DIR (official PSP2 SDK) the sources are compiled with psp2cgc and the result also refreshes
# shaders/gxp/, so the committed binaries follow the sources. Without it the committed binaries are used.

if(NOT DEFINED SCE_PSP2_SDK_DIR AND DEFINED ENV{SCE_PSP2_SDK_DIR})
  set(SCE_PSP2_SDK_DIR "$ENV{SCE_PSP2_SDK_DIR}" CACHE PATH "Official PSP2 SDK root (optional, provides psp2cgc)")
endif()

if(SCE_PSP2_SDK_DIR)
  find_program(PSP2CGC psp2cgc
    PATHS "${SCE_PSP2_SDK_DIR}/host_tools/bin"
    NO_DEFAULT_PATH)
  if(NOT PSP2CGC)
    message(FATAL_ERROR "psp2cgc not found under ${SCE_PSP2_SDK_DIR}/host_tools/bin")
  endif()
  message(STATUS "Shaders: compiled with ${PSP2CGC}")
else()
  message(STATUS "Shaders: prebuilt binaries from shaders/gxp")
endif()

set(NFS3_SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${NFS3_SHADER_BINARY_DIR}")

# nfs3_add_shader(<list-var> <name> <sce_vp_psp2|sce_fp_psp2>)
# Adds <name>.gxp -> <name>_gxp.c (symbols nfs3_shader_<name>, nfs3_shader_<name>_size).
function(nfs3_add_shader out_var name profile)
  set(src "${CMAKE_SOURCE_DIR}/shaders/${name}.cg")
  set(prebuilt "${CMAKE_SOURCE_DIR}/shaders/gxp/${name}.gxp")
  set(csrc "${NFS3_SHADER_BINARY_DIR}/${name}_gxp.c")
  if(SCE_PSP2_SDK_DIR)
    set(gxp "${NFS3_SHADER_BINARY_DIR}/${name}.gxp")
    add_custom_command(
      OUTPUT "${gxp}"
      COMMAND "${PSP2CGC}" -profile ${profile} -O3 -Wperf -o "${gxp}" "${src}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${gxp}" "${prebuilt}"
      DEPENDS "${src}"
      COMMENT "Compiling shader ${name}.cg (${profile})"
      VERBATIM)
  else()
    set(gxp "${prebuilt}")
    if(NOT EXISTS "${gxp}")
      message(FATAL_ERROR "Missing prebuilt shader ${gxp}; set SCE_PSP2_SDK_DIR to compile it.")
    endif()
  endif()
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
