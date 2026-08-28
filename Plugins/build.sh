#!/bin/bash
# クローン直後からビルドするためのスクリプト。
#
#   git clone --recurse-submodules <repo>
#   cd Nuked-SC55/Plugins && ./build.sh
#
# JUCE 9 のモジュールは同じバージョンの Projucer でしか保存できないため、
# サブモジュールの Projucer を先にビルドしてから Xcode プロジェクトを生成する。
set -euo pipefail
cd "$(dirname "$0")"

JUCE=../3rdparty/JUCE
CONFIG=${1:-Release}
PROJUCER="$JUCE/extras/Projucer/Builds/MacOSX/build/Release/Projucer.app/Contents/MacOS/Projucer"

if [ ! -d "$JUCE/modules" ]; then
    echo "JUCE のサブモジュールがありません。以下を実行してください:"
    echo "  git submodule update --init --recursive"
    exit 1
fi

if [ ! -x "$PROJUCER" ]; then
    echo "==> Projucer をビルド ($(cd "$JUCE" && git describe --tags 2>/dev/null || echo unknown))"
    xcodebuild -project "$JUCE/extras/Projucer/Builds/MacOSX/Projucer.xcodeproj" \
               -configuration Release -target "Projucer - App" build | tail -1
fi

echo "==> Xcode プロジェクトを生成"
"$PROJUCER" --resave Nuked-SC55.jucer

# 署名は既定でアドホック（誰でもビルドできる）。配布用に自分のチームで署名したい
# ときだけ、リポジトリを汚さずに外から渡す:
#   SC55_TEAM_ID=XXXXXXXXXX ./build.sh
SIGN_ARGS=()
if [ -n "${SC55_TEAM_ID:-}" ]; then
    echo "==> Team $SC55_TEAM_ID で署名"
    SIGN_ARGS=(DEVELOPMENT_TEAM="$SC55_TEAM_ID" CODE_SIGN_STYLE=Automatic)
fi

echo "==> ビルド ($CONFIG)"
# 前回の archive が残したシンボリックリンクがあると出力先を作れない
find "Builds/MacOSX/build/$CONFIG" -maxdepth 1 -type l -delete 2>/dev/null || true
xcodebuild -project Builds/MacOSX/SC-55.xcodeproj \
           -target "SC-55 - Standalone Plugin" -configuration "$CONFIG" \
           ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} build | tail -1

echo
echo "完了: Plugins/Builds/MacOSX/build/$CONFIG/SC-55.app"
echo "ROM は SC-55 v1.x の5ファイル (sc55_rom1.bin, sc55_rom2.bin, sc55_waverom1-3.bin) を"
echo "同じフォルダに置き、初回起動時に選択してください。"
