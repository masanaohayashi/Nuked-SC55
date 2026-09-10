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
    ${ROOT}/Plugins/Source/MidiFilePlayer.cpp
    ${ROOT}/Plugins/Source/RcpFilePlayer.cpp
    ${ROOT}/Plugins/Source/WrdFilePlayer.cpp
    ${ROOT}/src/common/rom_loader.cpp
    ${BACKEND}/rom.cpp ${BACKEND}/rom_io.cpp ${BACKEND}/standard_romsets.cpp
    ${BACKEND}/file_io.cpp ${BACKEND}/file_hashing.cpp
    ${BACKEND}/diagnostics.cpp)
target_include_directories(sc55-native-engine-check PRIVATE "${ROOT}/src/common" "${ROOT}/Plugins/Source")
target_link_libraries(sc55-native-engine-check PRIVATE sc55-native-engine)

set(SC55_ROM_DIRECTORY "" CACHE PATH "Optional local SC-55 v1.21 ROM directory for the native-only check")
set(SC55_MONO_REUSE_SONG "" CACHE FILEPATH "Optional local ANMDLYGS.RCP for mono completion regression")
set(SC55_MONO_REPLENISH_SONG "" CACHE FILEPATH "Optional local 55KTIZKE.RCP for mono replenishment regression")
enable_testing()
add_executable(sc55-pcm-voice-bank-check pcm_voice_bank_test.cpp)
target_link_libraries(sc55-pcm-voice-bank-check PRIVATE sc55-native-engine)
add_test(NAME pcm-voice-bank COMMAND sc55-pcm-voice-bank-check)
add_executable(sc55-meter-decay-check meter_decay_test.cpp)
target_compile_features(sc55-meter-decay-check PRIVATE cxx_std_20)
add_test(NAME meter-decay COMMAND sc55-meter-decay-check)
add_executable(sc55-voice-set-check voice_set_test.cpp)
target_compile_features(sc55-voice-set-check PRIVATE cxx_std_20)
target_include_directories(sc55-voice-set-check PRIVATE "${BACKEND}")
add_test(NAME voice-set COMMAND sc55-voice-set-check)
add_executable(sc55-voice-capacity-check voice_capacity_test.cpp)
target_compile_features(sc55-voice-capacity-check PRIVATE cxx_std_20)
target_include_directories(sc55-voice-capacity-check PRIVATE "${BACKEND}")
add_test(NAME voice-capacity COMMAND sc55-voice-capacity-check)
if(SC55_ROM_DIRECTORY)
    enable_testing()
    add_test(NAME native-without-h8
        COMMAND sc55-native-engine-check "${SC55_ROM_DIRECTORY}")
    add_test(NAME native-polyphony
        COMMAND sc55-native-engine-check "${SC55_ROM_DIRECTORY}" polyphony)
    if(SC55_MONO_REUSE_SONG)
        add_test(NAME native-mono-completion
            COMMAND sc55-native-engine-check "${SC55_ROM_DIRECTORY}" song-notes "${SC55_MONO_REUSE_SONG}")
        set_tests_properties(native-mono-completion PROPERTIES ENVIRONMENT "SC55_PROBE_SECONDS=15")
    endif()
    if(SC55_MONO_REPLENISH_SONG)
        add_test(NAME native-mono-replenishment
            COMMAND sc55-native-engine-check "${SC55_ROM_DIRECTORY}" song-notes "${SC55_MONO_REPLENISH_SONG}")
        set_tests_properties(native-mono-replenishment PROPERTIES
            ENVIRONMENT "SC55_PROBE_MONO_EXCERPT=1;SC55_PROBE_VOICES=48;SC55_PROBE_SECONDS=4")
    endif()
endif()
