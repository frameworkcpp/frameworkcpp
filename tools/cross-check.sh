#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Compiles the framework for the architectures the CI emulates.
#
# The CI builds those architectures under QEMU user-mode, which is slow and
# only proves that the code compiles. This script does the same check without
# the emulation: it fetches the cross toolchains, unpacks them without root
# and compiles one translation unit that exercises the whole public API.
#
# The point is the static_asserts. They state the real-time guarantees of the
# framework — lock-free atomics on the hot path — and they are evaluated by
# the compiler of the target architecture, so this script answers the only
# question that matters: does the guarantee hold there, or not?
#
# Usage:
#   tools/cross-check.sh              # every architecture
#   tools/cross-check.sh armv7 s390x  # only these
#
# Requires: apt-get, dpkg-deb, and network access. Nothing is installed on the
# system: the packages are unpacked under a temporary directory.

set -euo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly WORK_DIR="${CROSS_CHECK_DIR:-${TMPDIR:-/tmp}/frameworkcpp-cross}"

# target:prefixo-debian-cross:gcc
readonly TARGETS=(
  "armv7:arm-linux-gnueabihf:arm-linux-gnueabihf"
  "s390x:s390x-linux-gnu:s390x-linux-gnu"
  "riscv64:riscv64-linux-gnu:riscv64-linux-gnu"
  "ppc64le:powerpc64le-linux-gnu:powerpc64le-linux-gnu"
)

# Paquetes debian por arquitectura. Los nombres no siguen un patron fijo
# (ppc64le es "powerpc64le" pero "ppc64el" en las libs de desarrollo), asi que
# se listan a mano.
packages_for() {
  local _target="$1" _prefix="$2" _triplet="$3"
  case "$_target" in
    armv7)
      echo "gcc-12-$_prefix cpp-12-$_prefix binutils-$_prefix g++-12-$_prefix \
libgcc-12-dev-armhf-cross libstdc++-12-dev-armhf-cross libc6-dev-armhf-cross \
linux-libc-dev-armhf-cross libgcc-s1-armhf-cross"
      ;;
    s390x)
      echo "gcc-12-$_prefix cpp-12-$_prefix binutils-$_prefix g++-12-$_prefix \
libgcc-12-dev-$_target-cross libstdc++-12-dev-$_target-cross libc6-dev-$_target-cross \
linux-libc-dev-$_target-cross"
      ;;
    riscv64)
      echo "gcc-12-$_prefix cpp-12-$_prefix binutils-$_prefix g++-12-$_prefix \
libgcc-12-dev-$_target-cross libstdc++-12-dev-$_target-cross libc6-dev-$_target-cross \
linux-libc-dev-$_target-cross"
      ;;
    ppc64le)
      echo "gcc-12-$_prefix cpp-12-$_prefix binutils-$_prefix g++-12-$_prefix \
libgcc-12-dev-ppc64el-cross libstdc++-12-dev-ppc64el-cross libc6-dev-ppc64el-cross \
linux-libc-dev-ppc64el-cross"
      ;;
  esac
}

fetch() {
  local _target="$1" _prefix="$2" _triplet="$3"
  local _dir="${WORK_DIR}/${_target}"
  if [ -x "${_dir}/usr/bin/${_triplet}-gcc-12" ]; then
    return 0
  fi
  mkdir -p "${_dir}"
  ( cd "${_dir}" && apt-get download -q $(packages_for "$_target" "$_prefix" "$_triplet") \
      >/dev/null 2>&1 )
  for _deb in "${_dir}"/*.deb; do
    dpkg-deb -x "$_deb" "${_dir}/root"
  done
  # liburing solo se usa en compilacion: sus cabeceras son independientes de la
  # arquitectura, asi que las del host sirven.
  if [ ! -f "${_dir}/root/uring/usr/include/liburing.h" ]; then
    ( cd "${_dir}" && apt-get download -q liburing-dev >/dev/null 2>&1 )
    dpkg-deb -x "${_dir}"/liburing-dev_*.deb "${_dir}/root/uring"
  fi
}

compile() {
  local _target="$1" _prefix="$2" _triplet="$3"
  local _dir="${WORK_DIR}/${_target}/root"
  local _gcc="${_dir}/usr/bin/${_triplet}-gcc-12"
  local _out="${WORK_DIR}/${_target}/cross_check.o"

  local _cxx_inc=""
  for _candidate in "${_dir}/usr/${_triplet}/include/c++/12" \
      "${_dir}/usr/${_triplet}-linux-gnu/include/c++/12"; do
    if [ -d "$_candidate" ]; then
      _cxx_inc="$_candidate"
      break
    fi
  done

  LD_LIBRARY_PATH="${_dir}/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}" \
  "$_gcc" -std=c++20 -O2 -Wall -Wextra -Wno-psabi -c \
    -B"${_dir}/usr/lib/gcc-cross/${_triplet}/12/" \
    -B"${_dir}/usr/bin/" \
    -x c++ \
    -I"${ROOT_DIR}/include" \
    -isystem "${_dir}/root/uring/usr/include" \
    -isystem "${_cxx_inc}" \
    -isystem "${_cxx_inc}/${_triplet}" \
    -isystem "${_dir}/usr/${_triplet}/include" \
    -isystem "${_dir}/usr/include" \
    "${ROOT_DIR}/tools/cross_check_main.cpp" \
    -o "${_out}"
}

main() {
  local -a _wanted=("$@")
  local _failures=0

  for _entry in "${TARGETS[@]}"; do
    IFS=: read -r _target _prefix _triplet <<<"${_entry}"
    if [ ${#_wanted[@]} -gt 0 ]; then
      local _found=0
      for _want in "${_wanted[@]}"; do
        [ "$_want" = "$_target" ] && _found=1
      done
      [ "$_found" -eq 1 ] || continue
    fi

    printf '%-10s ' "${_target}"
    if fetch "${_target}" "${_prefix}" "${_triplet}" && compile "${_target}" "${_prefix}" "${_triplet}"; then
      echo "ok (los static_assert se cumplen)"
    else
      echo "FALLO"
      _failures=$((_failures + 1))
    fi
  done

  if [ "${_failures}" -ne 0 ]; then
    echo "${_failures} arquitectura(s) fallaron"
    return 1
  fi
  echo "todas las arquitecturas compilan y cumplen los asserts"
}

main "$@"
