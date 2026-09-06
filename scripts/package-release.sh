#!/usr/bin/env bash
# Build every desktop package and create one draft GitHub Release.
#
# macOS is built locally, Windows is built through Parallels Desktop, and Linux
# is built with Docker Buildx. The release is always created as a draft so it
# can be inspected before publishing.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
MACOS_SCRIPT="${SCRIPT_DIR}/macos/package-release.sh"
WINDOWS_SCRIPT="${SCRIPT_DIR}/windows/package-release.sh"
LINUX_SCRIPT="${SCRIPT_DIR}/linux/package-release.sh"
CONFIG_FILE="${SCRIPT_DIR}/macos/config.env"

if [[ -f "$CONFIG_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$CONFIG_FILE"
fi

: "${RELEASE_BRANCH:=master}"
: "${RELEASE_REMOTE:=}"
: "${GH_REPO:=}"
: "${PARALLELS_VM_NAME:=Windows 11}"
: "${PARALLELS_WINDOWS_REPO_ROOT:=C:\Mac\Home\Documents\src\Nuked-SC55-jcmoyer}"

TAG_OVERRIDE=""
VERSION_OVERRIDE=""
MACOS_IDENTITY_OVERRIDE=""
TEAM_ID_OVERRIDE=""
NOTARY_PROFILE_OVERRIDE=""

JUCER_FILE="${REPO_ROOT}/Plugins/Nuked-SC55.jucer"
DIST_DIR="${REPO_ROOT}/dist"

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
Usage: ./scripts/package-release.sh [options]

Builds every desktop package and creates one draft GitHub Release:
  macOS   universal arm64 + x86_64 DMG
  Windows x64 + ARM64 installers through Parallels Desktop
  Linux   x64 + arm64 tar.gz archives through Docker Buildx

The script refuses to build if the working tree is dirty or if HEAD is not
exactly equal to the configured release branch on the configured remote.

Options:
  --version VERSION        Release version (default: Plugins/Nuked-SC55.jucer)
  --tag vX.Y.Z             Release tag (default: v<version>)
  --remote NAME            Git remote (default: scripts/macos/config.env or auto)
  --repo OWNER/REPO        GitHub repository (default: config or remote)
  --identity NAME          macOS Developer ID Application identity
  --team-id ID             Apple Developer Team ID
  --notary-profile NAME    macOS notarytool Keychain profile
  --vm NAME                Parallels VM name or UUID (default: Windows 11)
  --windows-repo PATH      Windows-side repository path
  -h, --help               Show this help

Machine-specific values can be configured in scripts/macos/config.env.
The GitHub Release is always created as a draft.
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --version)
        [[ $# -ge 2 ]] || die "--version requires an argument"
        VERSION_OVERRIDE="$2"
        shift 2
        ;;
      --tag)
        [[ $# -ge 2 ]] || die "--tag requires an argument"
        TAG_OVERRIDE="$2"
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
      --identity)
        [[ $# -ge 2 ]] || die "--identity requires an argument"
        MACOS_IDENTITY_OVERRIDE="$2"
        shift 2
        ;;
      --team-id)
        [[ $# -ge 2 ]] || die "--team-id requires an argument"
        TEAM_ID_OVERRIDE="$2"
        shift 2
        ;;
      --notary-profile)
        [[ $# -ge 2 ]] || die "--notary-profile requires an argument"
        NOTARY_PROFILE_OVERRIDE="$2"
        shift 2
        ;;
      --vm)
        [[ $# -ge 2 ]] || die "--vm requires an argument"
        PARALLELS_VM_NAME="$2"
        shift 2
        ;;
      --windows-repo)
        [[ $# -ge 2 ]] || die "--windows-repo requires an argument"
        PARALLELS_WINDOWS_REPO_ROOT="$2"
        shift 2
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

check_git_state() {
  local current_branch status_text remote_head local_head

  current_branch="$(git -C "$REPO_ROOT" branch --show-current)"
  [[ "$current_branch" == "$RELEASE_BRANCH" ]] \
    || die "release must run on branch '$RELEASE_BRANCH' (current: ${current_branch:-detached HEAD})"

  status_text="$(git -C "$REPO_ROOT" status --porcelain --untracked-files=all)"
  [[ -z "$status_text" ]] || {
    printf '%s\n' "$status_text" >&2
    die "working tree is not clean; commit all changes before releasing"
  }

  git -C "$REPO_ROOT" fetch --quiet "$RELEASE_REMOTE" "$RELEASE_BRANCH" \
    || die "could not fetch $RELEASE_REMOTE/$RELEASE_BRANCH"
  local_head="$(git -C "$REPO_ROOT" rev-parse HEAD)"
  remote_head="$(git -C "$REPO_ROOT" rev-parse "$RELEASE_REMOTE/$RELEASE_BRANCH")"
  [[ "$local_head" == "$remote_head" ]] \
    || die "HEAD is not pushed to $RELEASE_REMOTE/$RELEASE_BRANCH; commit and push before releasing"

  RELEASE_COMMIT="$local_head"
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
    die "GitHub Release already exists: $GH_REPO/$RELEASE_TAG"
  fi
}

check_parallels_vm() {
  local status_output vm_status

  if ! status_output="$(prlctl status "$PARALLELS_VM_NAME" 2>&1)"; then
    die "could not query Parallels VM '${PARALLELS_VM_NAME}': ${status_output}"
  fi

  vm_status="${status_output##* }"
  [[ "$vm_status" == "running" ]] || {
    die "Parallels VM '${PARALLELS_VM_NAME}' is not running (status: ${vm_status}); start it before releasing"
  }
}

check_docker() {
  docker buildx version >/dev/null 2>&1 \
    || die "Docker Buildx is required; install/start Docker Desktop first"
  docker info >/dev/null 2>&1 \
    || die "Docker is not running; start Docker Desktop first"
}

assert_source_unchanged() {
  local current_head status_text

  current_head="$(git -C "$REPO_ROOT" rev-parse HEAD)"
  [[ "$current_head" == "$RELEASE_COMMIT" ]] \
    || die "HEAD changed while building; refusing to release a different commit"
  status_text="$(git -C "$REPO_ROOT" status --porcelain --untracked-files=all)"
  [[ -z "$status_text" ]] \
    || die "working tree changed while building; refusing to release"
}

run_macos_package() {
  local -a args

  args=(
    --package-only
    --architectures "arm64 x86_64"
    --version "$VERSION"
    --tag "$RELEASE_TAG"
    --remote "$RELEASE_REMOTE"
    --repo "$GH_REPO"
  )
  [[ -n "$MACOS_IDENTITY_OVERRIDE" ]] && args+=(--identity "$MACOS_IDENTITY_OVERRIDE")
  [[ -n "$TEAM_ID_OVERRIDE" ]] && args+=(--team-id "$TEAM_ID_OVERRIDE")
  [[ -n "$NOTARY_PROFILE_OVERRIDE" ]] && args+=(--notary-profile "$NOTARY_PROFILE_OVERRIDE")

  [[ -x "$MACOS_SCRIPT" ]] || die "missing executable macOS package script: $MACOS_SCRIPT"
  "$MACOS_SCRIPT" "${args[@]}"
}

run_windows_package() {
  [[ -x "$WINDOWS_SCRIPT" ]] || die "missing executable Windows package script: $WINDOWS_SCRIPT"
  "$WINDOWS_SCRIPT" \
    --vm "$PARALLELS_VM_NAME" \
    --windows-repo "$PARALLELS_WINDOWS_REPO_ROOT" \
    --architecture all \
    --configuration Release \
    --version "$VERSION"
}

run_linux_package() {
  [[ -x "$LINUX_SCRIPT" ]] || die "missing executable Linux package script: $LINUX_SCRIPT"
  "$LINUX_SCRIPT" \
    --architecture all \
    --configuration Release \
    --version "$VERSION"
}

check_artifacts() {
  local artifact

  ARTIFACTS=(
    "${DIST_DIR}/SC-55-${VERSION}-macOS.dmg"
    "${DIST_DIR}/SC-55-Linux-x64-${VERSION}.tar.gz"
    "${DIST_DIR}/SC-55-Linux-arm64-${VERSION}.tar.gz"
    "${DIST_DIR}/SC-55 Windows x64 ${VERSION} Setup.exe"
    "${DIST_DIR}/SC-55 Windows ARM64 ${VERSION} Setup.exe"
  )

  for artifact in "${ARTIFACTS[@]}"; do
    [[ -f "$artifact" ]] || die "release artifact was not created: $artifact"
  done

  for artifact in \
    "${DIST_DIR}/SC-55-Linux-x64-${VERSION}.tar.gz" \
    "${DIST_DIR}/SC-55-Linux-arm64-${VERSION}.tar.gz"; do
    tar -tzf "$artifact" | grep -F 'bin/nuked-sc55' >/dev/null \
      || die "Linux archive is missing nuked-sc55: $artifact"
    tar -tzf "$artifact" | grep -F 'bin/nuked-sc55-render' >/dev/null \
      || die "Linux archive is missing nuked-sc55-render: $artifact"
  done
}

create_draft_release() {
  local -a release_args

  check_git_state
  assert_source_unchanged

  log "Creating and pushing tag: $RELEASE_TAG"
  git -C "$REPO_ROOT" tag -a "$RELEASE_TAG" "$RELEASE_COMMIT" \
    -m "SC-55 $RELEASE_TAG"
  git -C "$REPO_ROOT" push "$RELEASE_REMOTE" "$RELEASE_TAG"

  release_args=(
    release create "$RELEASE_TAG"
    "${ARTIFACTS[@]}"
    --repo "$GH_REPO"
    --verify-tag
    --title "SC-55 $RELEASE_TAG"
    --generate-notes
    --draft
  )

  log "Creating draft GitHub Release: $GH_REPO/$RELEASE_TAG"
  gh "${release_args[@]}"
}

main() {
  local version

  parse_args "$@"

  require_cmd git
  require_cmd gh
  require_cmd docker
  require_cmd prlctl
  require_cmd xcodebuild
  require_cmd codesign
  require_cmd hdiutil
  require_cmd ditto
  require_cmd security
  require_cmd xcrun
  require_cmd tar
  require_cmd grep

  [[ -f "$JUCER_FILE" ]] || die "missing project file: $JUCER_FILE"
  resolve_release_remote

  version="${VERSION_OVERRIDE:-$(read_jucer_version)}"
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] \
    || die "invalid release version: $version"
  VERSION="$version"

  RELEASE_TAG="${TAG_OVERRIDE:-v${VERSION}}"
  [[ "$RELEASE_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] \
    || die "invalid release tag: $RELEASE_TAG"
  if [[ "$RELEASE_TAG" != "v${VERSION}" \
    && "$RELEASE_TAG" != "v${VERSION}-"* \
    && "$RELEASE_TAG" != "v${VERSION}."* ]]; then
    die "release tag must match the version (${VERSION}): ${RELEASE_TAG}"
  fi

  check_git_state
  gh auth status >/dev/null 2>&1 || die "gh is not authenticated; run gh auth login first"
  resolve_github_repo
  check_release_target
  check_docker
  check_parallels_vm

  log "Building macOS package"
  run_macos_package
  assert_source_unchanged

  log "Building Windows packages"
  run_windows_package
  assert_source_unchanged

  log "Building Linux packages"
  run_linux_package
  assert_source_unchanged

  check_artifacts
  create_draft_release

  cat <<EOF

SC-55 draft release ready
  tag     : $RELEASE_TAG
  commit  : $RELEASE_COMMIT
  repo    : $GH_REPO
  remote  : $RELEASE_REMOTE
  assets  : ${#ARTIFACTS[@]}
EOF
  for artifact in "${ARTIFACTS[@]}"; do
    printf '  - %s\n' "$artifact"
  done
}

main "$@"
