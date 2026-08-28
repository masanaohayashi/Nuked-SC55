cmake_minimum_required(VERSION 3.10)
project(NukedSC55 VERSION 0.2.0 LANGUAGES CXX)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/Utf8Main)

# 必要なモジュールをインクルード
include(FindPkgConfig)
include(GNUInstallDirs)
include(CheckCXXCompilerFlag)
include(CheckFunctionExists)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# C++11を有効化
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 出力ディレクトリ設定
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 最適化と警告オプション
if(NOT MSVC AND NOT APPLE)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2 -fdata-sections -ffunction-sections")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 -fdata-sections -ffunction-sections")
endif()

if(NOT MSVC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()

# SDL2の検出とリンク
find_package(SDL2 REQUIRED)
if(TARGET SDL2::SDL2)
    set(SDL2_LIBRARIES SDL2::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    include_directories(${SDL2_INCLUDE_DIRS})
    link_directories(${SDL2_LIBRARY_DIRS})
endif()

# SDL2_mixerの検出とリンク
find_package(SDL2_mixer REQUIRED)
if(TARGET SDL2_mixer::SDL2_mixer)
    set(SDL2_MIXER_LIBRARIES SDL2_mixer::SDL2_mixer)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2_mixer REQUIRED SDL2_mixer)
    include_directories(${SDL2_mixer_INCLUDE_DIRS})
    link_directories(${SDL2_mixer_LIBRARY_DIRS})
    set(SDL2_MIXER_LIBRARIES ${SDL2_mixer_LIBRARIES})
endif()

# ソースファイルの設定
set(SC55_SRC
    src/mcu.cpp src/mcu.h
    src/lcd.cpp src/lcd.h src/lcd_font.h
    src/mcu_interrupt.cpp src/mcu_interrupt.h
    src/mcu_opcodes.cpp src/mcu_opcodes.h
    src/mcu_timer.cpp src/mcu_timer.h
    src/midi.h
    src/pcm.cpp src/pcm.h
    src/submcu.cpp src/submcu.h
    src/utils/files.cpp src/utils/files.h
)

# RtMidiの設定
if(USE_RTMIDI)
    if(USE_SYSTEM_RTMIDI)
        pkg_check_modules(SYSTEM_RTMIDI rtmidi REQUIRED)
        include_directories(${SYSTEM_RTMIDI_INCLUDE_DIRS})
        list(APPEND SC55_SRC src/midi_rtmidi.cpp)
    else()
        include(RtMidi)
    endif()
endif()

# 実行可能ファイルの作成
add_executable(nuked-sc55 ${SC55_SRC})

# リンク設定
target_link_libraries(nuked-sc55 PRIVATE ${SDL2_LIBRARIES} ${SDL2_MIXER_LIBRARIES})

if(USE_RTMIDI AND USE_SYSTEM_RTMIDI)
    target_link_libraries(nuked-sc55 PRIVATE ${SYSTEM_RTMIDI_LIBRARIES})
endif()

if(APPLE)
    find_library(COREAUDIO_LIBRARY CoreAudio)
    target_link_libraries(nuked-sc55 PRIVATE ${COREAUDIO_LIBRARY})
endif()

# ROMファイルのコピー設定
set(SC55_INSTALL_FILES)
macro(copy_rom_target TargetName TargetFile)
    add_custom_target(sc55_copy_${TargetName} ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${TargetFile}")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${TargetFile}"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/data/${TargetFile}"
                "${CMAKE_CURRENT_BINARY_DIR}/${TargetFile}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/${TargetFile}"
    )
    list(APPEND SC55_INSTALL_FILES ${CMAKE_CURRENT_BINARY_DIR}/${TargetFile})
endmacro()

copy_rom_target(back_data back.data)

# インストール設定
install(TARGETS nuked-sc55
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")

install(FILES ${SC55_INSTALL_FILES}
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/nuked-sc55")
