#!/usr/bin/env bash
# Build and archive the Linux command-line frontends using Docker Buildx.
#
# The script is intended to run on macOS with Docker Desktop. Each requested
# architecture is built inside its matching Linux container platform, so an
# Apple Silicon Mac can produce both linux/arm64 and linux/amd64 archives.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
DOCKERFILE="${REPO_ROOT}/docker/linux-release/Dockerfile"
DIST_DIR="${REPO_ROOT}/dist"
BUILD_ROOT="${DIST_DIR}/.linux-release"

ARCHITECTURE_REQUEST="all"
CONFIGURATION="Release"
VERSION_OVERRIDE=""
FORCE=0
CLEAN=0
NO_CACHE=1

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
Usage: ./scripts/linux/package-release.sh [options]

Builds the Linux standard frontend and renderer in Docker, then creates one
tar.gz archive for each requested architecture.

Options:
  --architecture ARCH    x64, arm64, or all (default: all)
  --configuration NAME   Release or Debug (default: Release)
  --version VERSION      Override the version read from CMakeLists.txt
  --force                Replace archives with the same name
  --clean                Remove intermediate Linux release output first
  --cache                Reuse Docker Buildx layers (faster, not the default)
  --no-cache             Build without using the Docker cache (default)
  -h, --help             Show this help

Examples:
  ./scripts/linux/package-release.sh
  ./scripts/linux/package-release.sh --architecture arm64
  ./scripts/linux/package-release.sh --architecture all --force

Archives are written to dist/:
  SC-55-Linux-x64-VERSION.tar.gz
  SC-55-Linux-arm64-VERSION.tar.gz
EOF
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --architecture)
        [[ $# -ge 2 ]] || die "--architecture requires an argument"
        ARCHITECTURE_REQUEST="$2"
        shift 2
        ;;
      --configuration)
        [[ $# -ge 2 ]] || die "--configuration requires an argument"
        CONFIGURATION="$2"
        shift 2
        ;;
      --version)
        [[ $# -ge 2 ]] || die "--version requires an argument"
        VERSION_OVERRIDE="$2"
        shift 2
        ;;
      --force)
        FORCE=1
        shift
        ;;
      --clean)
        CLEAN=1
        shift
        ;;
      --no-cache)
        NO_CACHE=1
        shift
        ;;
      --cache)
        NO_CACHE=0
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

read_project_version() {
  local version

  version="$(sed -n \
    's/^project[[:space:]]*(nuked-sc55[[:space:]]VERSION[[:space:]]\([^[:space:]]*\).*/\1/p' \
    "${REPO_ROOT}/CMakeLists.txt" | head -n 1)"
  [[ -n "$version" ]] || die "could not read the version from CMakeLists.txt"
  printf '%s' "$version"
}

validate_args() {
  case "$ARCHITECTURE_REQUEST" in
    x64|arm64|all) ;;
    *) die "invalid architecture: ${ARCHITECTURE_REQUEST} (expected x64, arm64, or all)" ;;
  esac

  case "$CONFIGURATION" in
    Debug|Release) ;;
    *) die "invalid configuration: ${CONFIGURATION} (expected Debug or Release)" ;;
  esac
}

set_architectures() {
  case "$ARCHITECTURE_REQUEST" in
    all)
      ARCHITECTURES=(x64 arm64)
      ;;
    *)
      ARCHITECTURES=("${ARCHITECTURE_REQUEST}")
      ;;
  esac
}

docker_platform() {
  case "$1" in
    x64) printf 'linux/amd64' ;;
    arm64) printf 'linux/arm64' ;;
    *) die "unsupported architecture: $1" ;;
  esac
}

archive_name() {
  printf 'SC-55-Linux-%s-%s.tar.gz' "$1" "$VERSION"
}

package_name() {
  printf 'SC-55-Linux-%s-%s' "$1" "$VERSION"
}

check_output_paths() {
  local architecture archive_path

  for architecture in "${ARCHITECTURES[@]}"; do
    archive_path="${DIST_DIR}/$(archive_name "$architecture")"
    if [[ ( -e "$archive_path" || -L "$archive_path" ) && "$FORCE" -eq 0 ]]; then
      die "archive already exists: ${archive_path} (use --force to replace it)"
    fi
  done
}

verify_binary() {
  local architecture="$1"
  local binary="$2"
  local file_info

  [[ -x "$binary" ]] || die "required executable is missing: ${binary}"
  file_info="$(file "$binary")"

  case "$architecture" in
    x64)
      printf '%s\n' "$file_info" | grep -Eiq 'x86-64|x86_64' \
        || die "${binary} is not an x86-64 Linux binary: ${file_info}"
      ;;
    arm64)
      printf '%s\n' "$file_info" | grep -Eiq 'aarch64|arm64' \
        || die "${binary} is not an arm64 Linux binary: ${file_info}"
      ;;
  esac
}

build_architecture() {
  local architecture="$1"
  local platform
  local archive_file
  local archive_path
  local package_dir
  local package
  local export_dir
  local source_revision
  local build_source
  local -a docker_args

  platform="$(docker_platform "$architecture")"
  archive_file="$(archive_name "$architecture")"
  archive_path="${DIST_DIR}/${archive_file}"
  package="$(package_name "$architecture")"
  package_dir="${BUILD_ROOT}/${package}"
  export_dir="${BUILD_ROOT}/export-${architecture}"
  source_revision="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
  build_source="linux-docker-${VERSION}-${architecture}-${source_revision}"

  rm -rf "$export_dir" "$package_dir"
  mkdir -p "$export_dir" "$package_dir"

  docker_args=(
    buildx build
    --platform "$platform"
    --file "$DOCKERFILE"
    --progress=plain
    --build-arg "BUILD_TYPE=${CONFIGURATION}"
    --build-arg "NUKED_SOURCE=${build_source}"
    --build-arg "NUKED_VERSION=${VERSION}"
    --output "type=local,dest=${export_dir}"
  )
  if [[ "$NO_CACHE" -eq 1 ]]; then
    docker_args+=(--no-cache)
  fi
  docker_args+=("$REPO_ROOT")

  log "Building ${platform} (${CONFIGURATION})"
  docker "${docker_args[@]}"

  verify_binary "$architecture" "${export_dir}/bin/nuked-sc55"
  verify_binary "$architecture" "${export_dir}/bin/nuked-sc55-render"

  cp -R "${export_dir}/bin" "${package_dir}/bin"
  if [[ -d "${export_dir}/share" ]]; then
    cp -R "${export_dir}/share" "${package_dir}/share"
  fi

  # The frontends look for ROMs in <package>/share/nuked-sc55 by default.
  mkdir -p "${package_dir}/share/nuked-sc55"
  cp "${REPO_ROOT}/README.md" "${package_dir}/README.md"
  cp "${REPO_ROOT}/LICENSE" "${package_dir}/LICENSE"
  cp "${REPO_ROOT}/THIRD_PARTY_NOTICES.md" "${package_dir}/THIRD_PARTY_NOTICES.md"
  cp "${REPO_ROOT}/CHANGELOG.md" "${package_dir}/CHANGELOG.md"

  {
    printf 'SC-55 Linux release\n'
    printf 'Version: %s\n' "$VERSION"
    printf 'Architecture: %s (%s)\n' "$architecture" "$platform"
    printf 'Configuration: %s\n' "$CONFIGURATION"
    printf 'Source revision: %s\n' "$source_revision"
    printf 'Built at (UTC): %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    printf '\nRun the standard frontend with: ./bin/nuked-sc55 --help\n'
    printf 'Run the renderer with: ./bin/nuked-sc55-render --help\n'
    printf '\nPlace ROM files in share/nuked-sc55 or pass --rom-directory.\n'
    printf 'SDL2 and RtMidi runtime libraries are required on the target Linux system.\n'
  } >"${package_dir}/BUILD-INFO.txt"

  if [[ "$FORCE" -eq 1 && ( -e "$archive_path" || -L "$archive_path" ) ]]; then
    rm -f "$archive_path"
  fi

  log "Creating ${archive_file}"
  COPYFILE_DISABLE=1 tar -czf "$archive_path" -C "$BUILD_ROOT" "$package"

  tar -tzf "$archive_path" | grep -F "${package}/bin/nuked-sc55" >/dev/null \
    || die "archive is missing nuked-sc55: ${archive_path}"
  tar -tzf "$archive_path" | grep -F "${package}/bin/nuked-sc55-render" >/dev/null \
    || die "archive is missing nuked-sc55-render: ${archive_path}"

  printf '%s\n' "${archive_path}"
}

main() {
  local architecture

  parse_args "$@"
  validate_args

  require_cmd docker
  require_cmd file
  require_cmd git
  require_cmd grep
  require_cmd sed
  require_cmd tar

  [[ -f "$DOCKERFILE" ]] || die "missing Dockerfile: ${DOCKERFILE}"
  docker buildx version >/dev/null 2>&1 \
    || die "Docker Buildx is required; install/start Docker Desktop first"
  docker info >/dev/null 2>&1 \
    || die "Docker is not running; start Docker Desktop first"

  VERSION="${VERSION_OVERRIDE:-$(read_project_version)}"
  [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]] \
    || die "invalid version: ${VERSION}"

  set_architectures
  check_output_paths

  mkdir -p "$DIST_DIR"
  if [[ "$CLEAN" -eq 1 && -d "$BUILD_ROOT" ]]; then
    log "Removing intermediate Linux release output"
    rm -rf "$BUILD_ROOT"
  fi
  mkdir -p "$BUILD_ROOT"

  for architecture in "${ARCHITECTURES[@]}"; do
    build_architecture "$architecture"
  done

  printf '\nLinux release archives ready:\n'
  for architecture in "${ARCHITECTURES[@]}"; do
    printf '  %s\n' "${DIST_DIR}/$(archive_name "$architecture")"
  done
}

main "$@"
