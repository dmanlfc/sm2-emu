cmake_minimum_required(VERSION 3.24)
project(m68kmake_host C)

# m68kmake is Musashi's opcode-table generator. It must run on the build host,
# so this tiny project is configured and built separately with the host
# toolchain (see cmake/Dependencies.cmake), never the target one. It is fed the
# path to Musashi's m68kmake.c via SM2_M68KMAKE_SRC.

if(NOT SM2_M68KMAKE_SRC)
    message(FATAL_ERROR "SM2_M68KMAKE_SRC (path to Musashi's m68kmake.c) is required")
endif()

add_executable(m68kmake "${SM2_M68KMAKE_SRC}")
set_target_properties(m68kmake PROPERTIES C_STANDARD 99)
if(MSVC)
    target_compile_definitions(m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)

    # MSVC 19.44 at /O2 drops bit 14 from every generated opcode mask and match,
    # which crashes m68k_init(). Strip the config's /O flags and build at /Od.
    foreach(flags
            CMAKE_C_FLAGS
            CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_MINSIZEREL)
        string(REGEX REPLACE "[-/]O[a-zA-Z0-9]+" "" ${flags} "${${flags}}")
    endforeach()
    target_compile_options(m68kmake PRIVATE /Od)
else()
    target_compile_options(m68kmake PRIVATE -w)
endif()

install(TARGETS m68kmake RUNTIME DESTINATION bin)
