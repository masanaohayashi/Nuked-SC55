cmake_minimum_required(VERSION 3.22)
project(sc55_lcd_rendering LANGUAGES C CXX)

add_subdirectory(../../3rdparty/JUCE juce EXCLUDE_FROM_ALL)
add_executable(sc55-lcd-rendering-check main.cpp)
target_compile_features(sc55-lcd-rendering-check PRIVATE cxx_std_17)
target_compile_definitions(sc55-lcd-rendering-check PRIVATE
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(sc55-lcd-rendering-check PRIVATE juce::juce_graphics)
enable_testing()
add_test(NAME lcd-rendering COMMAND sc55-lcd-rendering-check)
