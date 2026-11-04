#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Runs the benchmark suite on this machine, the way the CI runs it.
#
# The CI job "benchmarks" (.github/workflows/ci.yml) configures the "bench"
# preset, builds it, runs the three binaries one after the other and uploads
# one JSON artifact per binary. This script does the same here: same preset,
# same binaries, same order, same JSON names, plus the timestamped report the
# CI log prints around each binary.
#
# Usage:
#   tools/run-benchmarks.sh                  # build (incremental) and run everything
#   tools/run-benchmarks.sh --no-build       # reuse the binaries already in build/bench
#   tools/run-benchmarks.sh --filter 'memory/.*'
#   tools/run-benchmarks.sh --baseline build/bench/results/2026-11-03T21-06-25Z
#
# Options:
#   --no-build        skip configure and build, use the existing binaries
#   --build-jobs N    parallel build jobs (default: number of CPUs)
#   --filter REGEX    --benchmark_filter applied to every binary
#   --json-dir DIR    directory for the JSON results
#                     (default: build/bench/results/<UTC timestamp>)
#   --no-json         run without writing JSON files
#   --baseline DIR    compare this run against the JSON files of another run
#   -h, --help        print this help

set -euo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly PRESET="bench"
readonly BENCH_DIR="${ROOT_DIR}/build/${PRESET}"
readonly BIN_DIR="${BENCH_DIR}/benchmarks"

readonly -a TARGETS=(
  "memory:frameworkcpp_memory_bench"
  "environment:frameworkcpp_environment_bench"
  "reactor:frameworkcpp_reactor_bench"
)

usage() {
  sed -n '2,/^$/s/^#\{1,\} \{0,1\}//p' "${BASH_SOURCE[0]}"
}

detect_arch() {
  case "$(uname -m)" in
    x86_64 | amd64) echo "amd64" ;;
    aarch64 | arm64) echo "arm64" ;;
    *) uname -m ;;
  esac
}

timestamp() {
  TZ=UTC date -Iseconds
}

build_suite() {
  (
    cd "${ROOT_DIR}"
    cmake --preset "${PRESET}"
    cmake --build --preset "${PRESET}" -j "${_jobs}"
  )
}

run_target() {
  local _name="$1" _binary_name="$2"
  local _binary="${BIN_DIR}/${_binary_name}"
  local _display="./build/${PRESET}/benchmarks/${_binary_name}"
  local -a _args=()

  if [ -n "${_filter}" ]; then
    _args+=("--benchmark_filter=${_filter}")
  fi
  if [ "${_write_json}" -eq 1 ]; then
    _args+=("--benchmark_out=${_json_dir}/${_name}-${_arch}.json")
    _args+=("--benchmark_out_format=json")
  fi

  if [ ! -x "${_binary}" ]; then
    echo "missing benchmark binary: ${_binary}" >&2
    echo "build it first: cmake --preset ${PRESET} && cmake --build --preset ${PRESET}" >&2
    return 1
  fi

  echo "=============== Run ${_display} \\"
  ( cd "${ROOT_DIR}" && TZ=UTC "./build/${PRESET}/benchmarks/${_binary_name}" "${_args[@]}" 2>&1 )
  timestamp
}

compare_runs() {
  local _base_dir="$1" _new_dir="$2"

  python3 - "${_base_dir}" "${_new_dir}" <<'PY'
import json
import os
import sys


def load(directory):
    samples = {}
    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".json"):
            continue
        with open(os.path.join(directory, entry), encoding="utf-8") as handle:
            try:
                report = json.load(handle)
            except ValueError:
                continue
        for bench in report.get("benchmarks", []):
            name = bench.get("name")
            if name is None or "real_time" not in bench:
                continue
            if bench.get("run_type") == "aggregate":
                samples[name] = bench["real_time"]
            else:
                samples.setdefault(name, []).append(bench["real_time"])
    for name, values in list(samples.items()):
        if isinstance(values, list):
            samples[name] = sum(values) / len(values)
    return samples


base = load(sys.argv[1])
new = load(sys.argv[2])
if not base or not new:
    print("no comparable benchmark results found")
    sys.exit(1)

print()
print(f"{'benchmark':<44}{'base':>12}{'now':>12}{'delta':>10}")
print("-" * 78)
for name in sorted(set(base) | set(new)):
    old = base.get(name)
    cur = new.get(name)
    if old is None or cur is None or old == 0:
        print(f"{name:<44}{'-':>12}{'-' if cur is None else f'{cur:.1f}':>12}{'-':>10}")
        continue
    delta = (cur - old) / old * 100.0
    mark = ""
    if delta > 10.0:
        mark = "  slower"
    elif delta < -10.0:
        mark = "  faster"
    print(f"{name:<44}{old:>12.1f}{cur:>12.1f}{delta:>9.1f}%{mark}")
PY
}

main() {
  local _build=1
  local _jobs
  local _filter=""
  local _json_dir=""
  local _write_json=1
  local _baseline=""
  local _arch

  _jobs="$(nproc 2>/dev/null || echo 2)"

  while [ $# -gt 0 ]; do
    case "$1" in
      --no-build)
        _build=0
        ;;
      --build-jobs)
        shift
        _jobs="${1:?--build-jobs needs a value}"
        ;;
      --build-jobs=*)
        _jobs="${1#*=}"
        ;;
      --filter)
        shift
        _filter="${1:?--filter needs a value}"
        ;;
      --filter=*)
        _filter="${1#*=}"
        ;;
      --json-dir)
        shift
        _json_dir="${1:?--json-dir needs a value}"
        ;;
      --json-dir=*)
        _json_dir="${1#*=}"
        ;;
      --no-json)
        _write_json=0
        ;;
      --baseline)
        shift
        _baseline="${1:?--baseline needs a value}"
        ;;
      --baseline=*)
        _baseline="${1#*=}"
        ;;
      -h | --help)
        usage
        return 0
        ;;
      *)
        echo "unknown option: $1" >&2
        usage >&2
        return 2
        ;;
    esac
    shift
  done

  _arch="$(detect_arch)"

  if [ -z "${_json_dir}" ]; then
    _json_dir="${BENCH_DIR}/results/$(TZ=UTC date +%Y-%m-%dT%H-%M-%SZ)"
  fi
  if [ "${_write_json}" -eq 1 ]; then
    mkdir -p "${_json_dir}"
  fi

  if [ "${_build}" -eq 1 ]; then
    build_suite
  fi

  local _entry _name _binary_name
  for _entry in "${TARGETS[@]}"; do
    IFS=: read -r _name _binary_name <<<"${_entry}"
    run_target "${_name}" "${_binary_name}"
  done

  echo
  echo "arch: ${_arch}"
  if [ -n "${_filter}" ]; then
    echo "filter: ${_filter}"
  fi
  if [ "${_write_json}" -eq 1 ]; then
    echo "json: ${_json_dir}"
  fi

  if [ -n "${_baseline}" ]; then
    if [ ! -d "${_baseline}" ]; then
      echo "baseline directory not found: ${_baseline}" >&2
      return 1
    fi
    compare_runs "${_baseline}" "${_json_dir}"
  fi
}

main "$@"
