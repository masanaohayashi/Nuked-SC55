#!/usr/bin/env bash
# Build, notarize, staple, and publish the SC-55 macOS release.
#
# The generated DMG contains:
#   SC-55.app
#   Applications -> /Applications
#
# The script intentionally refuses to run with a dirty worktree. A release
# must be created from the exact commit that was built and notarized.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
CONFIG_FILE="${SCRIPT_DIR}/config.env"

if [[ -f "$CONFIG_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$CONFIG_FILE"
fi

: "${APP_IDENTITY:=Developer ID Application: Masanao Hayashi (P5G28RMWUN)}"
: "${TEAM_ID:=P5G28RMWUN}"
: "${NOTARY_PROFILE:=Notalization2021}"
: "${BUILD_CONFIGURATION:=Release}"
: "${BUILD_ARCHS:=arm64 x86_64}"
: "${RELEASE_BRANCH:=master}"
: "${RELEASE_REMOTE:=}"
: "${GH_REPO:=}"

TAG_OVERRIDE=""
DRAFT_RELEASE=0

JUCER_FILE="${REPO_ROOT}/Plugins/Nuked-SC55.jucer"
XCODE_PROJECT="${REPO_ROOT}/Plugins/Builds/MacOSX/SC-55.xcodeproj"
MACOS_PROJECT_DIR="${REPO_ROOT}/Plugins/Builds/MacOSX"
APP_ENTITLEMENTS="${MACOS_PROJECT_DIR}/Standalone_Plugin.entitlements"
APPEX_ENTITLEMENTS="${MACOS_PROJECT_DIR}/AUv3_AppExtension.entitlements"

log() {
  printf '==> %s\n' "$*" >&2
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

usage() {
  cat <<'EOF'
Usage: ./scripts/macos/package-release.sh [options]

Builds a signed Universal macOS app, creates a DMG containing SC-55.app and
an Applications-folder link, notarizes and staples the DMG, then publishes a
GitHub Release with the DMG attached.

Options:
  --tag vX.Y.Z           Override the Git tag (default: v<version>)
  --identity NAME        Developer ID Application identity
  --team-id ID           Apple Developer Team ID
  --notary-profile NAME  notarytool Keychain profile
  --remote NAME          Git remote used for fetch/tag checks and tag push
  --repo OWNER/REPO      GitHub repository (default: gh repo view result)
  --draft                Leave the GitHub Release as a draft
  -h, --help             Show this help

The following environment variables can also be set in scripts/macos/config.env:
  APP_IDENTITY, TEAM_ID, NOTARY_PROFILE, BUILD_CONFIGURATION,
  BUILD_ARCHS, RELEASE_BRANCH, RELEASE_REMOTE, GH_REPO
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --tag)
        [[ $# -ge 2 ]] || die "--tag requires an argument"
        TAG_OVERRIDE="$2"
        shift 2
        ;;
      --identity)
        [[ $# -ge 2 ]] || die "--identity requires an argument"
        APP_IDENTITY="$2"
        shift 2
        ;;
      --team-id)
        [[ $# -ge 2 ]] || die "--team-id requires an argument"
        TEAM_ID="$2"
        shift 2
        ;;
      --notary-profile)
        [[ $# -ge 2 ]] || die "--notary-profile requires an argument"
        NOTARY_PROFILE="$2"
        shift 2
        ;;
      --remote)
        [[ $# -ge 2 ]] || die "--remote requires an argument"
        RELEASE_REMOTE="$2"
        shift 2
        ;;
      --repo)
        [[ $# -ge 2 ]] || die "--repo requires an argument"
        GH_REPO="$2"
        shift 2
        ;;
      --draft)
        DRAFT_RELEASE=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "unknown argument: $1 (try --help)"
        ;;
    esac
  done
}

read_jucer_version() {
  local version
  version="$(tr '\n' ' ' <"$JUCER_FILE" \
    | sed -n 's/.*<JUCERPROJECT[^>]* version="\([^"]*\)".*/\1/p' \
    | head -n 1)"
  [[ -n "$version" ]] || die "could not read the version from $JUCER_FILE"
  printf '%s' "$version"
}

resolve_release_remote() {
  local candidate

  if [[ -n "$RELEASE_REMOTE" ]]; then
    git -C "$REPO_ROOT" remote get-url "$RELEASE_REMOTE" >/dev/null 2>&1 \
      || die "configured release remote does not exist: $RELEASE_REMOTE"
    return
  fi

  if git -C "$REPO_ROOT" remote get-url origin >/dev/null 2>&1; then
    RELEASE_REMOTE="origin"
    return
  fi

  if git -C "$REPO_ROOT" remote get-url personal >/dev/null 2>&1; then
    RELEASE_REMOTE="personal"
    return
  fi

  candidate="$(git -C "$REPO_ROOT" remote | head -n 1)"
  [[ -n "$candidate" ]] || die "no Git remote is configured; set RELEASE_REMOTE first"
  RELEASE_REMOTE="$candidate"
}

resolve_github_repo() {
  local remote_url

  [[ -n "$GH_REPO" ]] && return

  remote_url="$(git -C "$REPO_ROOT" remote get-url "$RELEASE_REMOTE")"
  case "$remote_url" in
    https://github.com/*|http://github.com/*)
      GH_REPO="${remote_url#*github.com/}"
      ;;
    git@github.com:*)
      GH_REPO="${remote_url#git@github.com:}"
      ;;
    ssh://git@github.com/*)
      GH_REPO="${remote_url#ssh://git@github.com/}"
      ;;
  esac

  GH_REPO="${GH_REPO%.git}"
  [[ "$GH_REPO" == */* ]] && return

  GH_REPO="$(gh repo view --json nameWithOwner --jq '.nameWithOwner')" \
    || die "could not determine GitHub repository"
}

check_repository() {
  local current_branch status_text remote_head submodules

  [[ -f "$JUCER_FILE" ]] || die "missing project file: $JUCER_FILE"
  [[ -d "$XCODE_PROJECT" ]] || die "missing generated Xcode project: $XCODE_PROJECT"

  current_branch="$(git -C "$REPO_ROOT" branch --show-current)"
  [[ "$current_branch" == "$RELEASE_BRANCH" ]] \
    || die "release must run on branch '$RELEASE_BRANCH' (current: '$current_branch')"

  status_text="$(git -C "$REPO_ROOT" status --porcelain --untracked-files=all)"
  [[ -z "$status_text" ]] || {
    printf '%s\n' "$status_text" >&2
    die "working tree is not clean; commit or revert the changes above first"
  }

  git -C "$REPO_ROOT" fetch --quiet "$RELEASE_REMOTE" "$RELEASE_BRANCH" \
    || die "could not fetch $RELEASE_REMOTE/$RELEASE_BRANCH"
  remote_head="$(git -C "$REPO_ROOT" rev-parse "$RELEASE_REMOTE/$RELEASE_BRANCH")"
  [[ "$(git -C "$REPO_ROOT" rev-parse HEAD)" == "$remote_head" ]] \
    || die "HEAD is not equal to $RELEASE_REMOTE/$RELEASE_BRANCH; push the intended commit first"

  git -C "$REPO_ROOT" submodule update --init --recursive \
    || die "could not initialize/update submodules"
  submodules="$(git -C "$REPO_ROOT" submodule status --recursive)"
  if printf '%s\n' "$submodules" | grep -qE '^[+-]'; then
    printf '%s\n' "$submodules" >&2
    die "a submodule is not checked out at the recorded commit"
  fi

  [[ -d "${REPO_ROOT}/3rdparty/JUCE/modules" ]] \
    || die "JUCE submodule is missing: 3rdparty/JUCE/modules"
  [[ -d "${REPO_ROOT}/3rdparty/R2JUCE/modules" ]] \
    || die "R2JUCE submodule is missing: 3rdparty/R2JUCE/modules"
}

check_release_target() {
  local remote_status

  if git -C "$REPO_ROOT" rev-parse --verify --quiet "refs/tags/$RELEASE_TAG" >/dev/null; then
    die "local tag already exists: $RELEASE_TAG"
  fi

  if git -C "$REPO_ROOT" ls-remote --exit-code --refs "$RELEASE_REMOTE" \
    "refs/tags/$RELEASE_TAG" >/dev/null 2>&1; then
    die "remote tag already exists: $RELEASE_TAG"
  else
    remote_status=$?
    [[ "$remote_status" -eq 2 ]] \
      || die "could not check whether the remote tag exists: $RELEASE_TAG"
  fi

  if gh release view "$RELEASE_TAG" --repo "$GH_REPO" >/dev/null 2>&1; then
    die "GitHub Release already exists: $RELEASE_TAG"
  fi
}

check_signing_identity() {
  local identities
  identities="$(security find-identity -v -p codesigning 2>/dev/null)" \
    || die "could not inspect code-signing identities"
  case "$identities" in
    *"$APP_IDENTITY"*) ;;
    *) die "Developer ID Application identity not found: $APP_IDENTITY" ;;
  esac
}

archive_app() {
  local archive_log="$WORK_DIR/xcodebuild-archive.log"

  log "Archiving SC-55 - Standalone Plugin (${BUILD_CONFIGURATION}, ${BUILD_ARCHS})"
  # The checked-in project has a custom Sign Target phase. Build the archive
  # with an ad-hoc signature and without install-time stripping; the final
  # Developer ID signatures are applied below after all binary transformations.
  if ! xcodebuild archive \
    -project "$XCODE_PROJECT" \
    -scheme "SC-55 - Standalone Plugin" \
    -configuration "$BUILD_CONFIGURATION" \
    -destination "generic/platform=macOS" \
    -archivePath "$ARCHIVE_PATH" \
    ONLY_ACTIVE_ARCH=NO \
    ARCHS="$BUILD_ARCHS" \
    VALID_ARCHS="$BUILD_ARCHS" \
    CODE_SIGN_STYLE=Manual \
    CODE_SIGN_IDENTITY=- \
    DEVELOPMENT_TEAM="$TEAM_ID" \
    ENABLE_HARDENED_RUNTIME=YES \
    CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO \
    OTHER_CODE_SIGN_FLAGS="--timestamp" \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    STRIP_INSTALLED_PRODUCT=NO \
    COPY_PHASE_STRIP=NO \
    2>&1 | tee "$archive_log"; then
    die "xcodebuild archive failed; see $archive_log"
  fi

  [[ -d "$APP_PATH" ]] || die "archive did not produce: $APP_PATH"
  [[ -d "$APP_PATH/Contents/PlugIns/SC-55.appex" ]] \
    || die "AUv3 app extension is missing from the archived app"

  log "Applying final Developer ID signatures (AUv3, then app)"
  codesign --force --sign "$APP_IDENTITY" \
    --verbose=4 --timestamp --options runtime \
    --entitlements "$APPEX_ENTITLEMENTS" \
    --generate-entitlement-der \
    "$APP_PATH/Contents/PlugIns/SC-55.appex"
  codesign --force --sign "$APP_IDENTITY" \
    --verbose=4 --timestamp --options runtime \
    --entitlements "$APP_ENTITLEMENTS" \
    --generate-entitlement-der \
    "$APP_PATH"

  log "Verifying Developer ID signature on the archived app"
  codesign --verify --deep --strict --verbose=2 "$APP_PATH"
  local signature_info
  signature_info="$(codesign --display --verbose=4 "$APP_PATH" 2>&1)" \
    || die "codesign display failed"
  case "$signature_info" in
    *"$APP_IDENTITY"*) ;;
    *) die "archived app is not signed with: $APP_IDENTITY" ;;
  esac
}

create_dmg() {
  log "Preparing DMG contents"
  mkdir -p "$DMG_STAGE"
  COPYFILE_DISABLE=1 ditto --norsrc --noextattr --noqtn \
    "$APP_PATH" "$DMG_STAGE/SC-55.app"
  ln -s /Applications "$DMG_STAGE/Applications"

  log "Creating DMG: $DMG_PATH"
  hdiutil create \
    -volname "$DMG_VOLUME_NAME" \
    -srcfolder "$DMG_STAGE" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -ov "$DMG_PATH" \
    >"$WORK_DIR/hdiutil-create.log" 2>&1 \
    || { cat "$WORK_DIR/hdiutil-create.log" >&2; die "hdiutil create failed"; }

  [[ -f "$DMG_PATH" ]] || die "DMG was not created: $DMG_PATH"

  log "Signing DMG"
  codesign --force --sign "$APP_IDENTITY" --timestamp "$DMG_PATH"
  codesign --verify --verbose=2 "$DMG_PATH"
  hdiutil verify "$DMG_PATH"
}

notarize_dmg() {
  local notary_output="$WORK_DIR/notary-submit.json"
  local submission_id

  log "Submitting DMG to Apple Notary Service (profile: $NOTARY_PROFILE)"
  if ! xcrun notarytool submit "$DMG_PATH" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait \
    --output-format json 2>&1 | tee "$notary_output"; then
    die "notarytool submit failed; see $notary_output"
  fi

  if ! grep -Eq '"status"[[:space:]]*:[[:space:]]*"Accepted"' "$notary_output"; then
    submission_id="$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
      "$notary_output" | head -n 1)"
    if [[ -n "$submission_id" ]]; then
      xcrun notarytool log "$submission_id" \
        --keychain-profile "$NOTARY_PROFILE" \
        "$WORK_DIR/notary-log.json" >/dev/null 2>&1 || true
    fi
    die "notarization was not accepted; see $notary_output and $WORK_DIR/notary-log.json"
  fi

  log "Stapling notarization ticket to DMG"
  xcrun stapler staple "$DMG_PATH"
  xcrun stapler validate "$DMG_PATH"
  codesign --verify --verbose=2 "$DMG_PATH"
  hdiutil verify "$DMG_PATH"
}

verify_dmg_contents() {
  local attach_log="$WORK_DIR/hdiutil-attach.log"

  MOUNT_POINT="$WORK_DIR/mounted-dmg"
  mkdir -p "$MOUNT_POINT"
  log "Checking DMG contents and Applications link"
  if ! hdiutil attach "$DMG_PATH" \
    -readonly -nobrowse -noautoopen -mountpoint "$MOUNT_POINT" \
    >"$attach_log" 2>&1; then
    cat "$attach_log" >&2
    die "could not mount the notarized DMG"
  fi
  MOUNTED=1

  [[ -d "$MOUNT_POINT/SC-55.app" ]] || die "DMG is missing SC-55.app"
  [[ -L "$MOUNT_POINT/Applications" ]] || die "DMG is missing Applications link"
  [[ "$(readlink "$MOUNT_POINT/Applications")" == "/Applications" ]] \
    || die "Applications link does not target /Applications"
  codesign --verify --deep --strict --verbose=2 "$MOUNT_POINT/SC-55.app"

  hdiutil detach "$MOUNT_POINT" >/dev/null \
    || hdiutil detach "$MOUNT_POINT" -force >/dev/null \
    || die "could not detach the DMG after verification"
  MOUNTED=0
}

publish_release() {
  local current_head
  local status_text
  local release_args

  current_head="$(git -C "$REPO_ROOT" rev-parse HEAD)"
  [[ "$current_head" == "$RELEASE_COMMIT" ]] \
    || die "HEAD changed while building; refusing to tag a different commit"
  status_text="$(git -C "$REPO_ROOT" status --porcelain --untracked-files=all)"
  [[ -z "$status_text" ]] \
    || die "working tree changed while building; refusing to publish"

  log "Creating and pushing tag: $RELEASE_TAG"
  git -C "$REPO_ROOT" tag -a "$RELEASE_TAG" "$RELEASE_COMMIT" \
    -m "SC-55 $RELEASE_TAG"
  git -C "$REPO_ROOT" push "$RELEASE_REMOTE" "$RELEASE_TAG"

  release_args=(
    release create "$RELEASE_TAG" "$DMG_PATH"
    --repo "$GH_REPO"
    --verify-tag
    --title "$RELEASE_TITLE"
    --generate-notes
  )
  if [[ "$DRAFT_RELEASE" -eq 1 ]]; then
    release_args+=(--draft)
  fi

  log "Creating GitHub Release: $GH_REPO/$RELEASE_TAG"
  gh "${release_args[@]}"
}

cleanup() {
  if [[ "${MOUNTED:-0}" -eq 1 && -n "${MOUNT_POINT:-}" ]]; then
    hdiutil detach "$MOUNT_POINT" >/dev/null 2>&1 \
      || hdiutil detach "$MOUNT_POINT" -force >/dev/null 2>&1 \
      || true
  fi
}

main() {
  local version

  parse_args "$@"

  require_cmd git
  require_cmd xcodebuild
  require_cmd codesign
  require_cmd hdiutil
  require_cmd ditto
  require_cmd security
  require_cmd xcrun
  require_cmd gh

  resolve_release_remote
  version="$(read_jucer_version)"
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] \
    || die "invalid version: $version"
  RELEASE_TAG="${TAG_OVERRIDE:-v${version}}"
  [[ "$RELEASE_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] \
    || die "invalid release tag: $RELEASE_TAG"
  if [[ "$RELEASE_TAG" != "v${version}" \
    && "$RELEASE_TAG" != "v${version}-"* \
    && "$RELEASE_TAG" != "v${version}."* ]]; then
    die "release tag must match the Nuked-SC55.jucer version (${version}): $RELEASE_TAG"
  fi
  RELEASE_TITLE="SC-55 ${RELEASE_TAG}"

  DIST_DIR="${REPO_ROOT}/dist"
  WORK_DIR="${DIST_DIR}/work/${RELEASE_TAG}"
  ARCHIVE_PATH="${WORK_DIR}/SC-55.xcarchive"
  APP_PATH="${ARCHIVE_PATH}/Products/Applications/SC-55.app"
  DMG_STAGE="${WORK_DIR}/dmg-root"
  DMG_VOLUME_NAME="SC-55 ${version}"
  DMG_PATH="${DIST_DIR}/SC-55-${version}-macOS.dmg"
  RELEASE_COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD)"
  MOUNT_POINT=""
  MOUNTED=0
  trap cleanup EXIT

  check_repository
  gh auth status >/dev/null 2>&1 || die "gh is not authenticated; run gh auth login first"
  resolve_github_repo
  check_release_target
  check_signing_identity

  mkdir -p "$DIST_DIR"
  [[ ! -e "$WORK_DIR" ]] || die "work directory already exists: $WORK_DIR"
  [[ ! -e "$DMG_PATH" ]] || die "DMG already exists: $DMG_PATH"
  mkdir -p "$WORK_DIR"

  archive_app
  create_dmg
  notarize_dmg
  verify_dmg_contents
  publish_release

  cat <<EOF

SC-55 release complete
  tag     : $RELEASE_TAG
  commit  : $RELEASE_COMMIT
  DMG     : $DMG_PATH
  repo    : $GH_REPO
  remote  : $RELEASE_REMOTE
  notary  : $NOTARY_PROFILE
EOF
  if [[ "$DRAFT_RELEASE" -eq 1 ]]; then
    printf '  status  : draft\n'
  else
    printf '  status  : published\n'
  fi
}

main "$@"
