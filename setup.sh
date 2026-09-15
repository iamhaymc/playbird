#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd "$SCRIPT_DIR" > /dev/null || exit 1
trap 'popd > /dev/null 2>&1 || true' EXIT

have_cmd() { command -v "$1" > /dev/null 2>&1; }

run_as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif have_cmd sudo; then
        sudo "$@"
    else
        echo "Root privileges are required to install build tools; install them manually or add sudo." >&2
        return 1
    fi
}

install_build_tools() {
    if have_cmd apt-get; then
        run_as_root apt-get update &&
            run_as_root apt-get install -y python3 clang libx11-dev libgl1-mesa-dev libglu1-mesa-dev
    elif have_cmd dnf; then
        run_as_root dnf install -y python3 clang libX11-devel mesa-libGL-devel mesa-libGLU-devel
    elif have_cmd yum; then
        run_as_root yum install -y python3 clang libX11-devel mesa-libGL-devel mesa-libGLU-devel
    elif have_cmd pacman; then
        run_as_root pacman -S --needed --noconfirm python clang libx11 mesa glu
    elif have_cmd apk; then
        run_as_root apk add --no-cache python3 clang musl-dev libx11-dev mesa-dev glu-dev
    elif have_cmd brew; then
        brew install python llvm
        export PATH="$(brew --prefix llvm)/bin:$PATH"
    else
        echo "No supported package manager found; install Python 3 and a C99 compiler manually." >&2
        return 1
    fi
}

if ! have_cmd python3 || { ! have_cmd cc && ! have_cmd clang && ! have_cmd gcc; }; then
    echo "Installing fly99 build tools..."
    install_build_tools || exit 1
fi

if ! have_cmd python3; then
    echo "Python 3 is not available on PATH." >&2
    exit 1
fi
if ! have_cmd cc && ! have_cmd clang && ! have_cmd gcc; then
    echo "No C compiler is available on PATH." >&2
    exit 1
fi

echo "Python: $(python3 --version)"
if have_cmd cc; then
    echo "C compiler: $(cc --version | head -n1)"
elif have_cmd clang; then
    echo "C compiler: $(clang --version | head -n1)"
else
    echo "C compiler: $(gcc --version | head -n1)"
fi
echo "fly99 build tools are ready."
