# Sequence file path regression

Exercises the production MIDI/RCP file loader with a minimal MIDI fixture under
ASCII, Japanese, Japanese-directory, and supplementary Unicode filenames.
Checks decoded events and verifies that missing-file errors retain the UTF-8
path. No ROMs or JUCE build are required.

```sh
cmake -S tools/sequence-files -B build/sequence-files
cmake --build build/sequence-files --config Release
ctest --test-dir build/sequence-files -C Release --output-on-failure
```

Run on Windows with a non-UTF-8 system code page (for example Japanese CP932)
to catch passing UTF-8 paths directly to the narrow CRT `fopen`. MSVC reads the
test source with `/utf-8`; that flag does not change the CRT's path encoding.
