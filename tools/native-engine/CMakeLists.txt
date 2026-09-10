cmake_minimum_required(VERSION 3.20)
project(sc55_native_engine LANGUAGES C CXX)

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BACKEND "${ROOT}/src/backend")

# NativeSynth/control are header-defined. The only compiled sound dependency
# is the existing PCM DSP and patch-data decoder/hash, including the explicit
# optional PCM comparison renderer.
# No Emulator, MCU, interrupt/kernel, sub-MCU, LCD, JUCE or host adapter sources.
add_library(sc55-native-engine STATIC
    ${BACKEND}/pcm.cpp
    ${BACKEND}/pcm_sim.cpp
    ${BACKEND}/sc55_patch.cpp
    ${BACKEND}/sha/sha224-256.c)
target_compile_features(sc55-native-engine PUBLIC cxx_std_20)
target_include_directories(sc55-native-engine PUBLIC "${BACKEND}")

# Filesystem/file hashing/ROM decoding belong to this setup-time consumer, not the
# sound library. Reuse the product's ROM loader and wave unscrambler.
add_executable(sc55-native-engine-check main.cpp
    ${ROOT}/src/common/rom_loader.cpp
    ${BACKEND}/rom.cpp ${BACKEND}/rom_io.cpp ${BACKEND}/standard_romsets.cpp
    ${BACKEND}/file_io.cpp ${BACKEND}/file_hashing.cpp
    ${BACKEND}/diagnostics.cpp)
target_include_directories(sc55-native-engine-check PRIVATE "${ROOT}/src/common")
target_link_libraries(sc55-native-engine-check PRIVATE sc55-native-engine)

set(SC55_ROM_DIRECTORY "" CACHE PATH "Optional local SC-55 v1.21 ROM directory for the native-only check")
if(SC55_ROM_DIRECTORY)
    enable_testing()
    add_test(NAME native-without-h8
        COMMAND sc55-native-engine-check "${SC55_ROM_DIRECTORY}")
endif()
