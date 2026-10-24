if(NUKED_TEST_ROMDIR)
    message(STATUS "Integration tests will run using roms in ${NUKED_TEST_ROMDIR}")
    add_subdirectory("integration")
else()
    message(WARNING "NUKED_TEST_ROMDIR not provided; integration tests will not be included!")
endif()

find_package(Catch2 3 REQUIRED)
add_executable(tests test_ringbuffer.cpp test_gain.cpp test_bitset.cpp test_bounded_vector.cpp)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain nuked-sc55-backend nuked-sc55-common)
target_compile_features(tests PRIVATE cxx_std_23)

include(Catch)
catch_discover_tests(tests)

# Header-only DSP regression check; also runs without JUCE or ROMs.
add_executable(stereo_dipole_test test_stereo_dipole.cpp)
target_compile_features(stereo_dipole_test PRIVATE cxx_std_17)
add_test(NAME stereo_dipole_test COMMAND stereo_dipole_test)
