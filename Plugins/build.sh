#!/bin/bash
# クローン直後からビルドするためのスクリプト。
#
#   git clone --recurse-submodules <repo>
#   cd Nuked-SC55/Plugins && ./build.sh
#
# XcodeプロジェクトとJuceLibraryCodeはリポジトリに固定しているため、
# このスクリプトは生成元ファイルを変更しない。
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

JUCE="$SCRIPT_DIR/../3rdparty/JUCE"
CONFIG=${1:-Release}
XCODE_PROJECT="$SCRIPT_DIR/Builds/MacOSX/SC-55.xcodeproj"
JUCE_HEADER="$SCRIPT_DIR/JuceLibraryCode/JuceHeader.h"
DERIVED_DATA_PATH="${SC55_DERIVED_DATA_PATH:-$SCRIPT_DIR/Builds/MacOSX/build/DerivedData}"

if [ ! -d "$JUCE/modules" ]; then
    echo "JUCE のサブモジュールがありません。以下を実行してください:"
    echo "  git submodule update --init --recursive"
    exit 1
fi

if [ ! -d "$XCODE_PROJECT" ]; then
    echo "生成済みXcodeプロジェクトがありません: $XCODE_PROJECT" >&2
    echo "リポジトリのBuilds/JuceLibraryCodeを取得してください。" >&2
    exit 1
fi

if [ ! -f "$JUCE_HEADER" ]; then
    echo "生成済みJuceLibraryCodeがありません: $JUCE_HEADER" >&2
    exit 1
fi

# 署名は既定でアドホック（誰でもビルドできる）。配布用に自分のチームで署名したい
# ときだけ、リポジトリを汚さずに外から渡す:
#   SC55_TEAM_ID=XXXXXXXXXX ./build.sh
SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY=-)
if [ -n "${SC55_TEAM_ID:-}" ]; then
    echo "==> Team $SC55_TEAM_ID で署名"
    SIGN_ARGS=(DEVELOPMENT_TEAM="$SC55_TEAM_ID" CODE_SIGN_STYLE=Automatic)
fi

echo "==> ビルド ($CONFIG)"
# 前回の archive が残したシンボリックリンクがあると出力先を作れない
find "$SCRIPT_DIR/Builds/MacOSX/build/$CONFIG" -maxdepth 1 -type l -delete 2>/dev/null || true
xcodebuild -project "$XCODE_PROJECT" \
           -scheme "SC-55 - Standalone Plugin" -configuration "$CONFIG" \
           -derivedDataPath "$DERIVED_DATA_PATH" \
           ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} \
           -destination 'platform=macOS' build

echo
echo "完了: Plugins/Builds/MacOSX/build/$CONFIG/SC-55.app"
echo "ROM は SC-55 v1.x の5ファイル (sc55_rom1.bin, sc55_rom2.bin, sc55_waverom1-3.bin) を"
echo "同じフォルダに置き、初回起動時に選択してください。"
