# Header-only DSP regression check; also runs without JUCE or ROMs.
add_executable(stereo_dipole_test test_stereo_dipole.cpp)
target_compile_features(stereo_dipole_test PRIVATE cxx_std_17)
add_test(NAME stereo_dipole_test COMMAND stereo_dipole_test)
