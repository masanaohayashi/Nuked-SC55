cmake_minimum_required(VERSION 3.19)
project(sc55_firmware_oracle LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BACKEND "${ROOT}/src/backend")
add_executable(sc55-firmware-oracle main.cpp
    ${BACKEND}/diagnostics.cpp ${BACKEND}/emu.cpp
    ${BACKEND}/file_hashing.cpp ${BACKEND}/file_io.cpp ${BACKEND}/lcd.cpp
    ${BACKEND}/mcu.cpp ${BACKEND}/mcu_interrupt.cpp ${BACKEND}/mcu_opcodes.cpp
    ${BACKEND}/mcu_timer.cpp ${BACKEND}/pcm.cpp ${BACKEND}/pcm_sim.cpp
    ${BACKEND}/rom.cpp ${BACKEND}/rom_io.cpp ${BACKEND}/standard_romsets.cpp
    ${BACKEND}/submcu.cpp ${BACKEND}/sc55_patch.cpp ${BACKEND}/sha/sha224-256.c
    ${ROOT}/src/common/rom_loader.cpp)
target_include_directories(sc55-firmware-oracle PRIVATE "${BACKEND}" "${ROOT}/src/common")
# Route only CPU writes through the observer. The PCM implementation is unchanged.
# This definition exists only in this standalone research build.
set_source_files_properties(${BACKEND}/mcu.cpp PROPERTIES COMPILE_DEFINITIONS "PCM_Write=Oracle_PCM_Write;SC55_ORACLE_H8_PROFILE=1")
enable_testing()
add_test(NAME uart-status COMMAND sc55-firmware-oracle --uart-status-test)
add_executable(sc55-patch-layout-test patch-layout-test.cpp ${BACKEND}/sc55_patch.cpp ${BACKEND}/sha/sha224-256.c)
target_include_directories(sc55-patch-layout-test PRIVATE "${BACKEND}")
add_test(NAME patch-layout COMMAND sc55-patch-layout-test)
add_executable(sc55-midi-decoder-test midi-decoder-test.cpp)
target_include_directories(sc55-midi-decoder-test PRIVATE "${BACKEND}")
add_test(NAME midi-decoder COMMAND sc55-midi-decoder-test)
add_executable(sc55-sysex-test sysex-test.cpp)
target_include_directories(sc55-sysex-test PRIVATE "${BACKEND}")
add_test(NAME sysex COMMAND sc55-sysex-test)
add_executable(sc55-sample-zone-test sample-zone-test.cpp)
target_include_directories(sc55-sample-zone-test PRIVATE "${BACKEND}")
add_test(NAME sample-zone COMMAND sc55-sample-zone-test)
add_executable(sc55-sound-data-test sound-data-test.cpp ${BACKEND}/sc55_patch.cpp ${BACKEND}/sha/sha224-256.c)
target_include_directories(sc55-sound-data-test PRIVATE "${BACKEND}")
add_test(NAME sound-data COMMAND sc55-sound-data-test)
add_executable(sc55-envelope-runner-test envelope-runner-test.cpp)
target_include_directories(sc55-envelope-runner-test PRIVATE "${BACKEND}")
add_test(NAME envelope-runner COMMAND sc55-envelope-runner-test)
add_executable(sc55-pitch-runner-test pitch-runner-test.cpp)
target_include_directories(sc55-pitch-runner-test PRIVATE "${BACKEND}")
add_test(NAME pitch-runner COMMAND sc55-pitch-runner-test)
add_test(NAME envelope-pcm COMMAND sc55-firmware-oracle --native-envelope-pcm-test)
set(SC55_NATIVE_TVA_TEST_DIRECTORY "" CACHE PATH "Local v1.21 ROM directory for native TVA dispatch test")
if(SC55_NATIVE_TVA_TEST_DIRECTORY)
    add_test(NAME native-tva COMMAND sc55-firmware-oracle --native-tva-test "${SC55_NATIVE_TVA_TEST_DIRECTORY}")
endif()
set(SC55_SOUND_DATA_TEST_ASSET "" CACHE FILEPATH "Local MD13 data-only asset for native voice/PCM integration (not distributed)")
if(SC55_SOUND_DATA_TEST_ASSET)
    if(NOT EXISTS "${SC55_SOUND_DATA_TEST_ASSET}")
        message(FATAL_ERROR "SC55_SOUND_DATA_TEST_ASSET does not exist")
    endif()
    add_test(NAME voice-control-pcm COMMAND sc55-firmware-oracle
        --native-voice-control-pcm-test "${SC55_SOUND_DATA_TEST_ASSET}")
endif()
set(SC55_WAVE_TEST_DIRECTORY "" CACHE PATH "Local v1.21 waveform ROM directory for native audio integration")
if(SC55_WAVE_TEST_DIRECTORY)
    if(NOT SC55_SOUND_DATA_TEST_ASSET)
        message(FATAL_ERROR "Wave output test requires SC55_SOUND_DATA_TEST_ASSET")
    endif()
    add_test(NAME native-wave-output COMMAND sc55-firmware-oracle
        --native-wave-output-test "${SC55_SOUND_DATA_TEST_ASSET}" "${SC55_WAVE_TEST_DIRECTORY}")
    add_test(NAME native-player COMMAND sc55-firmware-oracle
        --native-player-test "${SC55_SOUND_DATA_TEST_ASSET}" "${SC55_WAVE_TEST_DIRECTORY}")
    add_test(NAME native-pcm-boundary COMMAND sc55-firmware-oracle
        --native-pcm-boundary-test "${SC55_WAVE_TEST_DIRECTORY}")
    add_test(NAME native-gs-modes COMMAND sc55-firmware-oracle
        --compare-native-modes "${SC55_SOUND_DATA_TEST_ASSET}" "${SC55_WAVE_TEST_DIRECTORY}")
    add_test(NAME native-rhythm-bulk COMMAND sc55-firmware-oracle
        --compare-native-rhythm-bulk "${SC55_SOUND_DATA_TEST_ASSET}" "${SC55_WAVE_TEST_DIRECTORY}")
endif()
