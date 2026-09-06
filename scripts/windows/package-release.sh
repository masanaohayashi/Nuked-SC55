#!/usr/bin/env bash
# Run the Windows release packager from macOS through Parallels Desktop.
#
# The repository is expected to be available inside the Windows guest through
# Parallels shared folders. The default mapping is the one used by this
# machine: the Mac home directory is mounted at C:\Mac\Home.

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
WINDOWS_SCRIPT="${SCRIPT_DIR}/package-release.ps1"

VM_NAME="${PARALLELS_VM_NAME:-Windows 11}"
WINDOWS_REPO_ROOT="${PARALLELS_WINDOWS_REPO_ROOT:-C:\Mac\Home\Documents\src\Nuked-SC55-jcmoyer}"
ARCHITECTURE_REQUEST="x64"
CONFIGURATION="Release"
VERSION_OVERRIDE=""
SKIP_BUILD=0
CLEAN=0

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
Usage: ./scripts/windows/package-release.sh [options]

Runs scripts/windows/package-release.ps1 inside a running Windows VM managed by
Parallels Desktop. The Windows repository is expected to be available through
the Parallels shared-folder mapping C:\Mac\Home by default.

Options:
  --vm NAME                Parallels VM name or UUID (default: Windows 11)
  --windows-repo PATH      Repository path inside Windows
                           (default: C:\Mac\Home\Documents\src\Nuked-SC55-jcmoyer)
  --architecture ARCH      x64, arm64, or all (default: x64)
  --configuration NAME     Release or Debug (default: Release)
  --version VERSION        Override the version read by the Windows script
  --skip-build             Package existing Windows build outputs
  --clean                  Remove the selected Windows build output first
  -h, --help               Show this help

Environment overrides:
  PARALLELS_VM_NAME, PARALLELS_WINDOWS_REPO_ROOT

Examples:
  ./scripts/windows/package-release.sh
  ./scripts/windows/package-release.sh --architecture arm64
  ./scripts/windows/package-release.sh --architecture all --clean
EOF
}

normalize_architecture() {
  case "$1" in
    x64|X64)
      printf 'x64'
      ;;
    arm64|ARM64)
      printf 'ARM64'
      ;;
    all|ALL)
      printf 'all'
      ;;
    *)
      die "invalid architecture: $1 (expected x64, arm64, or all)"
      ;;
  esac
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --vm)
        [[ $# -ge 2 ]] || die "--vm requires an argument"
        VM_NAME="$2"
        shift 2
        ;;
      --windows-repo)
        [[ $# -ge 2 ]] || die "--windows-repo requires an argument"
        WINDOWS_REPO_ROOT="$2"
        shift 2
        ;;
      --architecture)
        [[ $# -ge 2 ]] || die "--architecture requires an argument"
        ARCHITECTURE_REQUEST="$(normalize_architecture "$2")"
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
      --skip-build)
        SKIP_BUILD=1
        shift
        ;;
      --clean)
        CLEAN=1
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

validate_args() {
  case "$CONFIGURATION" in
    Debug|Release) ;;
    *) die "invalid configuration: ${CONFIGURATION} (expected Debug or Release)" ;;
  esac

  [[ -f "$WINDOWS_SCRIPT" ]] || die "missing Windows package script: ${WINDOWS_SCRIPT}"
}

set_architectures() {
  case "$ARCHITECTURE_REQUEST" in
    all)
      ARCHITECTURES=(x64 ARM64)
      ;;
    *)
      ARCHITECTURES=("$ARCHITECTURE_REQUEST")
      ;;
  esac
}

check_vm() {
  local status_output vm_status

  if ! status_output="$(prlctl status "$VM_NAME" 2>&1)"; then
    die "could not query Parallels VM '${VM_NAME}': ${status_output}"
  fi

  vm_status="${status_output##* }"
  [[ "$vm_status" == "running" ]] || {
    die "Parallels VM '${VM_NAME}' is not running (status: ${vm_status}); start it before packaging"
  }
}

package_architecture() {
  local architecture="$1"
  local windows_script_path
  local -a powershell_args

  windows_script_path="${WINDOWS_REPO_ROOT}\\scripts\\windows\\package-release.ps1"
  powershell_args=(-Architecture "$architecture" -Configuration "$CONFIGURATION")

  [[ -n "$VERSION_OVERRIDE" ]] && powershell_args+=(-Version "$VERSION_OVERRIDE")
  [[ "$SKIP_BUILD" -eq 1 ]] && powershell_args+=(-SkipBuild)
  [[ "$CLEAN" -eq 1 ]] && powershell_args+=(-Clean)

  log "Packaging Windows ${architecture} in Parallels VM '${VM_NAME}'"
  prlctl exec "$VM_NAME" --current-user powershell.exe \
    -NoLogo \
    -NoProfile \
    -NonInteractive \
    -ExecutionPolicy Bypass \
    -File "$windows_script_path" \
    "${powershell_args[@]}" \
    || die "Windows package script failed for ${architecture}"
}

main() {
  parse_args "$@"
  validate_args
  set_architectures
  require_cmd prlctl
  check_vm

  for architecture in "${ARCHITECTURES[@]}"; do
    package_architecture "$architecture"
  done

  log "Windows installers are available in ${REPO_ROOT}/dist"
  find "${REPO_ROOT}/dist" -maxdepth 1 -type f -name 'SC-55 Windows * Setup.exe' -print | sort
}

main "$@"
