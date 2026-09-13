#!/usr/bin/env bash
set -euo pipefail

required=(git make gcc g++ rsync python3 wget tar xz m4 patch sed awk magick)
missing=()
for command_name in "${required[@]}"; do
	command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
done

if ((${#missing[@]})); then
	echo "Missing host tools: ${missing[*]}" >&2
	exit 1
fi

cmake_command="${RECONCLAVE_HOST_CMAKE:-cmake}"
cmake_version="$("$cmake_command" --version 2>/dev/null | awk 'NR == 1 {print $3}')"
if [[ -z "$cmake_version" ]]; then
	echo "CMake is missing; install CMake 3.18 through 3.31." >&2
	exit 1
fi
cmake_major="${cmake_version%%.*}"
cmake_minor="${cmake_version#*.}"
cmake_minor="${cmake_minor%%.*}"
if ((cmake_major > 3 || (cmake_major == 3 && cmake_minor > 31))); then
	echo "CMake $cmake_version is newer than this Buildroot release supports." >&2
	echo "Set RECONCLAVE_HOST_CMAKE to a standalone CMake 3.18-3.31 binary." >&2
	unsupported=1
fi

gcc_major="$(gcc -dumpversion | cut -d. -f1)"
if ((gcc_major >= 15)); then
	echo "Warning: GCC $gcc_major is known to break the SDK's GNU m4 1.4.19 host build." >&2
	echo "Use the documented Ubuntu 22.04/24.04 build environment for a clean build." >&2
	unsupported=1
fi

if [[ "${unsupported:-0}" == 1 && "${RECONCLAVE_ALLOW_UNSUPPORTED_HOST:-0}" != 1 ]]; then
	echo "Refusing a long build that is known to fail; set RECONCLAVE_ALLOW_UNSUPPORTED_HOST=1 to override." >&2
	exit 1
fi
echo "Host check complete. CMake $cmake_version; GCC $(gcc -dumpfullversion)."
