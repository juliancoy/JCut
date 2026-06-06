#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DURATION_SEC=15
FREQ=999
CALL_GRAPH="fp"
PID=""
OUT_DIR=""
PERF_EVENT="cycles:u"

usage() {
  cat <<'EOF'
Usage:
  scripts/capture_callstacks.sh [options] --pid <PID>
  scripts/capture_callstacks.sh [options] -- <command> [args...]

Options:
  --pid <PID>             Attach to an existing process.
  --duration <seconds>    Capture duration when using --pid. Default: 15.
  --freq <hz>             Sampling frequency. Default: 999.
  --call-graph <mode>     perf call graph mode: fp or dwarf. Default: fp.
  --dwarf                 Shortcut for --call-graph dwarf.
  --event <event>         perf event. Default: cycles:u.
  --out-dir <path>        Output directory. Default: profiling/callstacks-<timestamp>.
  -h, --help              Show this help.

Artifacts:
  perf.data               Raw perf recording.
  perf-script.txt         Human-readable perf script output.
  llm-collapsed-stacks.txt
                          Unique sampled stacks as "count<TAB>frame;frame;...".
  llm-callstacks.txt      Reviewable stack blocks with numbered function frames.
  summary.txt             Capture settings and artifact paths.
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pid)
      [[ $# -ge 2 ]] || die "--pid requires a value"
      PID="$2"
      shift 2
      ;;
    --duration)
      [[ $# -ge 2 ]] || die "--duration requires a value"
      DURATION_SEC="$2"
      shift 2
      ;;
    --freq)
      [[ $# -ge 2 ]] || die "--freq requires a value"
      FREQ="$2"
      shift 2
      ;;
    --call-graph)
      [[ $# -ge 2 ]] || die "--call-graph requires a value"
      CALL_GRAPH="$2"
      shift 2
      ;;
    --dwarf)
      CALL_GRAPH="dwarf"
      shift
      ;;
    --event)
      [[ $# -ge 2 ]] || die "--event requires a value"
      PERF_EVENT="$2"
      shift 2
      ;;
    --out-dir)
      [[ $# -ge 2 ]] || die "--out-dir requires a value"
      OUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

COMMAND=("$@")

[[ -n "${PID}" || ${#COMMAND[@]} -gt 0 ]] || die "provide --pid <PID> or -- <command>"
[[ -z "${PID}" || ${#COMMAND[@]} -eq 0 ]] || die "use either --pid or a command, not both"
[[ "${CALL_GRAPH}" == "fp" || "${CALL_GRAPH}" == "dwarf" ]] || die "--call-graph must be fp or dwarf"
[[ "${FREQ}" =~ ^[0-9]+$ ]] || die "--freq must be an integer"
[[ "${DURATION_SEC}" =~ ^[0-9]+$ ]] || die "--duration must be an integer"

command -v perf >/dev/null 2>&1 || die "perf is not installed or not in PATH"
command -v awk >/dev/null 2>&1 || die "awk is not installed or not in PATH"

if [[ -n "${PID}" ]]; then
  [[ "${PID}" =~ ^[0-9]+$ ]] || die "--pid must be an integer"
  [[ -d "/proc/${PID}" ]] || die "process ${PID} does not exist"
fi

if [[ -z "${OUT_DIR}" ]]; then
  OUT_DIR="${REPO_ROOT}/profiling/callstacks-$(date +%Y%m%d-%H%M%S)"
elif [[ "${OUT_DIR}" != /* ]]; then
  OUT_DIR="${REPO_ROOT}/${OUT_DIR}"
fi

mkdir -p "${OUT_DIR}"

PERF_DATA="${OUT_DIR}/perf.data"
PERF_SCRIPT="${OUT_DIR}/perf-script.txt"
COLLAPSED_STACKS="${OUT_DIR}/llm-collapsed-stacks.txt"
LLM_STACKS="${OUT_DIR}/llm-callstacks.txt"
SUMMARY="${OUT_DIR}/summary.txt"

run_perf_record() {
  if [[ -n "${PID}" ]]; then
    perf record \
      -e "${PERF_EVENT}" \
      -F "${FREQ}" \
      -g \
      --call-graph "${CALL_GRAPH}" \
      -o "${PERF_DATA}" \
      -p "${PID}" \
      -- sleep "${DURATION_SEC}"
  else
    perf record \
      -e "${PERF_EVENT}" \
      -F "${FREQ}" \
      -g \
      --call-graph "${CALL_GRAPH}" \
      -o "${PERF_DATA}" \
      -- "${COMMAND[@]}"
  fi
}

PERF_RECORD_STATUS=0
run_perf_record || PERF_RECORD_STATUS=$?
if [[ "${PERF_RECORD_STATUS}" -ne 0 && ! -s "${PERF_DATA}" ]]; then
  cat >&2 <<EOF
error: perf recording failed

Common causes:
  - insufficient perf permissions
  - kernel perf_event_paranoid setting is too restrictive
  - missing debug/unwind information for the selected call graph mode

Try:
  sudo sysctl kernel.perf_event_paranoid=1
  scripts/capture_callstacks.sh --dwarf ...
EOF
  exit 1
fi
if [[ "${PERF_RECORD_STATUS}" -ne 0 ]]; then
  cat >&2 <<EOF
warning: perf record exited with status ${PERF_RECORD_STATUS}, but ${PERF_DATA} was written.
         Continuing to generate reports from the partial capture. This commonly happens when the profiled app crashes or aborts.
EOF
fi

perf script -i "${PERF_DATA}" > "${PERF_SCRIPT}"

# Convert perf's sample blocks into a compact unique-stack format.
# Stack order is preserved as emitted by perf script, usually leaf frame first.
awk '
function trim(s) {
  sub(/^[[:space:]]+/, "", s)
  sub(/[[:space:]]+$/, "", s)
  return s
}

function flush_stack() {
  if (depth == 0) {
    return
  }

  stack = frames[1]
  for (i = 2; i <= depth; i++) {
    stack = stack ";" frames[i]
  }

  counts[stack]++
  depth = 0
  delete frames
}

/^[[:space:]]*[0-9a-fA-F]+[[:space:]]+/ {
  line = $0
  sub(/^[[:space:]]*[0-9a-fA-F]+[[:space:]]+/, "", line)
  sub(/[[:space:]]+\(.*/, "", line)
  sub(/\+0x[0-9a-fA-F]+.*/, "", line)
  line = trim(line)

  if (line != "" && line != "[unknown]") {
    depth++
    frames[depth] = line
  }
  next
}

/^[^[:space:]]/ {
  flush_stack()
  next
}

END {
  flush_stack()
  for (stack in counts) {
    print counts[stack] "\t" stack
  }
}
' "${PERF_SCRIPT}" | sort -rn > "${COLLAPSED_STACKS}"

awk -F '\t' '
BEGIN {
  print "# LLM-callstack summary"
  print "# Stack order is preserved from perf script, usually leaf frame first."
  print "# Each block is one unique sampled stack."
  print ""
}

{
  stack_number++
  print "stack: " stack_number
  print "sample_count: " $1
  n = split($2, frames, ";")
  print "frame_count: " n
  print "frames:"
  for (i = 1; i <= n; i++) {
    print "  " i ". " frames[i]
  }
  print ""
}
' "${COLLAPSED_STACKS}" > "${LLM_STACKS}"

{
  echo "JCut call stack capture"
  echo "timestamp: $(date --iso-8601=seconds)"
  echo "repo_root: ${REPO_ROOT}"
  echo "event: ${PERF_EVENT}"
  echo "frequency_hz: ${FREQ}"
  echo "call_graph: ${CALL_GRAPH}"
  echo "perf_record_status: ${PERF_RECORD_STATUS}"
  if [[ -n "${PID}" ]]; then
    echo "mode: attach"
    echo "pid: ${PID}"
    echo "duration_sec: ${DURATION_SEC}"
  else
    echo "mode: launch"
    printf 'command:'
    printf ' %q' "${COMMAND[@]}"
    echo
  fi
  echo
  echo "artifacts:"
  echo "  perf_data: ${PERF_DATA}"
  echo "  perf_script: ${PERF_SCRIPT}"
  echo "  llm_collapsed_stacks: ${COLLAPSED_STACKS}"
  echo "  llm_callstacks: ${LLM_STACKS}"
} > "${SUMMARY}"

cat "${SUMMARY}"
