cmake_minimum_required(VERSION 3.20)
project(gs-55 LANGUAGES C CXX)

# The plug-in itself builds from Plugins/ (Projucer). This builds the native
# sound engine's checks and the header-only DSP tests.
enable_testing()
add_subdirectory(tools/native-engine)
add_subdirectory(test)
