cmake_minimum_required(VERSION 3.20)
project(sc55_cpu_roles LANGUAGES C CXX OBJCXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD 20)
option(SC55_NATIVE_IO_AUDIT "Count remaining native compatibility I/O (diagnostics only)" OFF)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(BACKEND "${ROOT}/src/backend")
# Read-only instruction/IRQ observers exist only in this diagnostic target.
set_source_files_properties(${BACKEND}/mcu.cpp PROPERTIES COMPILE_DEFINITIONS SC55_ORACLE_H8_PROFILE=1)
set_source_files_properties(${BACKEND}/mcu_interrupt.cpp ${BACKEND}/mcu_opcodes.cpp
    PROPERTIES COMPILE_DEFINITIONS SC55_ORACLE_H8_INTERRUPTS=1)
add_executable(sc55-cpu-roles main.cpp
    ${ROOT}/Plugins/Source/MidiFilePlayer.cpp
    ${ROOT}/Plugins/Source/RcpFilePlayer.cpp
    ${ROOT}/Plugins/Source/WrdFilePlayer.cpp
    ${ROOT}/Plugins/Source/NukedSC55Emulator.cpp
    ${BACKEND}/diagnostics.cpp ${BACKEND}/emu.cpp
    ${BACKEND}/file_hashing.cpp ${BACKEND}/file_io.cpp ${BACKEND}/lcd.cpp
    ${BACKEND}/mcu.cpp ${BACKEND}/mcu_interrupt.cpp ${BACKEND}/mcu_opcodes.cpp
    ${BACKEND}/mcu_timer.cpp ${BACKEND}/pcm.cpp ${BACKEND}/pcm_sim.cpp
    ${BACKEND}/rom.cpp ${BACKEND}/rom_io.cpp ${BACKEND}/standard_romsets.cpp
    ${BACKEND}/submcu.cpp ${BACKEND}/sc55_patch.cpp ${BACKEND}/sha/sha224-256.c
    ${ROOT}/src/common/rom_loader.cpp
    ${ROOT}/3rdparty/JUCE/modules/juce_core/juce_core.mm
    ${ROOT}/3rdparty/JUCE/modules/juce_core/juce_core_CompilationTime.cpp)
target_include_directories(sc55-cpu-roles PRIVATE "${BACKEND}" "${ROOT}/src/common"
    "${ROOT}/Plugins/Source" "${ROOT}/3rdparty/JUCE/modules")
target_compile_definitions(sc55-cpu-roles PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    $<$<BOOL:${SC55_NATIVE_IO_AUDIT}>:SC55_NATIVE_IO_AUDIT=1>
    JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0
    $<$<CONFIG:Debug>:DEBUG=1> $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>)
target_compile_options(sc55-cpu-roles PRIVATE -g -fno-omit-frame-pointer)
target_link_libraries(sc55-cpu-roles PRIVATE "-framework Cocoa" "-framework CoreServices"
    "-framework IOKit" "-framework Security" z iconv)
