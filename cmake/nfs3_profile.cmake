# NFS3_PROFILE build: compiles instrumented copies of the generated code (tools/profile_instrument.py, written to
# <build>/profile) instead of src/nfs3hp/disassembly, and adds the sampler (src/platform/prof.cpp).
# Included at the end of CMakeLists.txt, after the targets exist.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(NFS3_PROFILE_SCRIPT ${CMAKE_SOURCE_DIR}/tools/profile_instrument.py)
get_target_property(nfs3_profile_sources nfs3_game SOURCES)
set(nfs3_profile_copies "")
foreach(source IN LISTS nfs3_profile_sources)
  get_filename_component(name ${source} NAME)
  set(copy ${CMAKE_BINARY_DIR}/profile/${name})
  add_custom_command(OUTPUT ${copy}
    COMMAND ${Python3_EXECUTABLE} ${NFS3_PROFILE_SCRIPT} ${source} ${copy}
    DEPENDS ${source} ${NFS3_PROFILE_SCRIPT}
    COMMENT "Instrumenting ${name}"
    VERBATIM)
  # Same per-file options as the original (the -Og of the light sources).
  get_source_file_property(options ${source} COMPILE_OPTIONS)
  if(options)
    set_source_files_properties(${copy} PROPERTIES COMPILE_OPTIONS "${options}")
  endif()
  list(APPEND nfs3_profile_copies ${copy})
endforeach()
set_property(TARGET nfs3_game PROPERTY SOURCES ${nfs3_profile_copies})
target_compile_options(nfs3_game PRIVATE "SHELL:-include \"${CMAKE_SOURCE_DIR}/include/platform/prof.h\"")

target_sources(nfs3_runtime PRIVATE src/platform/prof.cpp)
target_compile_definitions(nfs3_runtime PRIVATE NFS3_PROFILE)
target_compile_definitions(NFS3VITA.elf PRIVATE NFS3_PROFILE)
# prof.cpp registers every guest thread for sampling around WinApplication::runThread.
target_link_options(NFS3VITA.elf PRIVATE -Wl,--wrap=_ZN5win3214WinApplication9runThreadERN3x863CPUEjjb)
