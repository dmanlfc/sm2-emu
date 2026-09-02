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
else()
    target_compile_options(m68kmake PRIVATE -w)
endif()

install(TARGETS m68kmake RUNTIME DESTINATION bin)
