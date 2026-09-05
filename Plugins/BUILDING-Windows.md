# Windows ARM64 Release

Use Visual Studio 2026 with the C++ desktop workload, MSVC ARM64 build tools,
and a Windows SDK installed.

1. Open `Builds/VisualStudio2026/SC-55.sln`.
2. Select `Release` and `ARM64` in the solution toolbar.
3. Build the solution. The Windows target is the standalone application.

Output: `Builds/VisualStudio2026/ARM64/Release/Standalone Plugin/SC-55.exe`.

From a Visual Studio Developer PowerShell, starting in the repository root:

```powershell
msbuild Plugins/Builds/VisualStudio2026/SC-55.sln /m /p:Configuration=Release /p:Platform=ARM64
```

To build the Windows installer (install Inno Setup first), run from the
repository root in an ordinary PowerShell:

```powershell
./scripts/windows/package-release.ps1 -Architecture x64
./scripts/windows/package-release.ps1 -Architecture ARM64
```

The installer is written directly to `dist/` and contains selectable
Standalone and VST3 components. Use `-SkipBuild` to package existing build
outputs or `-Clean` to remove the selected architecture's generated output
before building. Install Inno Setup with `winget install --id JRSoftware.InnoSetup -e` if `ISCC.exe` is not found.

If the solution is missing or needs regeneration, open `Nuked-SC55.jucer`
with a Projucer built from this repository's `3rdparty/JUCE` and save it.
The installed JUCE 8 Projucer does not support this project's VS2026 exporter.
The exporter sets `/utf-8` so MSVC reads the UTF-8 sources correctly regardless
of the Windows system code page. Keep this flag when changing build settings.

On Windows ARM64, use the ARM64-native Visual Studio Developer PowerShell
and MSBuild (`MSBuild/Current/Bin/arm64/MSBuild.exe`). The compiler and linker
paths in the build log should contain `HostArm64/arm64`.

Windows Release disables link-time optimisation because MSVC 14.51.36231
fails with internal compiler error C1001 in JUCE `juce_Path.h` during ARM64
link-time code generation, with both x86-hosted and ARM64-native tools.
Normal Release compiler optimisation and fast math remain enabled.
