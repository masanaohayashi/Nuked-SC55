cmake_minimum_required(VERSION 3.20)
project(sc55_lcd_meter_oracle LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BACKEND "${ROOT}/src/backend")
# Separate from the large firmware oracle: no shortcuts or profiling hooks.
add_executable(sc55-lcd-meter-oracle main.cpp
    ${BACKEND}/diagnostics.cpp
    ${BACKEND}/emu.cpp
    ${BACKEND}/file_hashing.cpp
    ${BACKEND}/file_io.cpp
    ${BACKEND}/lcd.cpp
    ${BACKEND}/mcu.cpp
    ${BACKEND}/mcu_interrupt.cpp
    ${BACKEND}/mcu_opcodes.cpp
    ${BACKEND}/mcu_timer.cpp
    ${BACKEND}/pcm.cpp
    ${BACKEND}/pcm_sim.cpp
    ${BACKEND}/rom.cpp
    ${BACKEND}/rom_io.cpp
    ${BACKEND}/standard_romsets.cpp
    ${BACKEND}/submcu.cpp
    ${BACKEND}/sc55_patch.cpp
    ${BACKEND}/sha/sha224-256.c
    ${ROOT}/src/common/rom_loader.cpp
    ${ROOT}/Plugins/Source/MidiFilePlayer.cpp
    ${ROOT}/Plugins/Source/RcpFilePlayer.cpp
    ${ROOT}/Plugins/Source/WrdFilePlayer.cpp)
target_include_directories(sc55-lcd-meter-oracle PRIVATE
    "${BACKEND}" "${ROOT}/src/common" "${ROOT}/Plugins/Source")
