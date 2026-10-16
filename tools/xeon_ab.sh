#!/usr/bin/env bash
# Tuning sweep for a 4-core / 8-thread CPU (Xeon E3-1245 v2): runs the game once per
# configuration with per-frame statistics, then prints a comparison against the baseline.
#
#   bash tools/xeon_ab.sh [SECONDS [PHASE...]]   (default 120 s per run; the game is closed automatically)
#   PHASE: base gc spin prep2 rec1 tex gcpress drain50 drain100 faultwin uffd nomotion sections
#   (base is always run first; no PHASE: all of them)
#
# Stand in the same spot (e.g. Hunter's Nightmare) for every run: do not move the camera
# between runs, or the numbers are not comparable. Results: out/ab_<name>_*.frames.csv/.log
set -uo pipefail
cd "$(dirname "$0")/.."
secs=${1:-120}
# Seconds dropped from the start of each summary (shader compiles). The frame log starts ~20 s after
# the launch, so a run of S seconds leaves S - 20 - skip seconds of frames: with the old fixed 60 an
# 80 s run kept 1-2 s and the baseline sometimes none. BB_AB_SKIP overrides.
skip=${BB_AB_SKIP:-40}
if (( secs - 20 - skip < 30 )); then
    echo "!! ${secs}s runs leave only $((secs - 20 - skip))s of frames after skipping ${skip}s; use 120 or set BB_AB_SKIP" >&2
fi

# play_ab.sh defaults to ../game_files/CUSA03173 while run.sh defaults to ../CUSA03173: find the
# folder that really holds eboot.bin (or use BB_GAME_DIR) before spending 8 minutes on failed launches.
if [[ -z ${BB_GAME_DIR:-} ]]; then
    for d in "$PWD/../game_files/CUSA03173" "$PWD/../CUSA03173"; do
        [[ -f $d/eboot.bin ]] && { BB_GAME_DIR=$d; break; }
    done
fi
if [[ ! -f ${BB_GAME_DIR:-/nonexistent}/eboot.bin ]]; then
    echo "No game folder found. Run: BB_GAME_DIR=/path/to/CUSA03173 bash tools/xeon_ab.sh" >&2
    exit 1
fi
export BB_GAME_DIR
echo "Game: $BB_GAME_DIR"

declare -a names=(base gc spin prep2 rec1 tex gcpress drain50 drain100 faultwin uffd nomotion sections)
if (( $# > 1 )); then
    names=(base "${@:2}")
fi
declare -A cfg=(
  [base]=""
  [gc]="BB_GC_BUDGET_MB=5000"
  [spin]="BB_GC_BUDGET_MB=5000 BB_GPU_SPIN_US=100 BB_PIPE_SPIN_US=150"
  [prep2]="BB_GC_BUDGET_MB=5000 BB_PREP_WORKERS=2"
  [rec1]="BB_GC_BUDGET_MB=5000 BB_VK_RECORD_THREADS=1"
  [tex]="BB_GC_BUDGET_MB=5000 BB_TEXTURE_HELPER=1"
  [gcpress]="BB_GC_BUDGET_MB=5000 BB_GC_PRESSURE_IDLE_SECONDS=20"
  # stage A sleeps in 50/100 us steps instead of a sched_yield loop while it waits for stage B
  [drain50]="BB_GC_BUDGET_MB=5000 BB_DRAIN_SLEEP_US=50"
  [drain100]="BB_GC_BUDGET_MB=5000 BB_DRAIN_SLEEP_US=100"
  # write-fault unprotect window 1 MiB (default 256 KiB), and userfaultfd write tracking
  [faultwin]="BB_GC_BUDGET_MB=5000 BB_FAULT_WINDOW=1024"
  [uffd]="BB_GC_BUDGET_MB=5000 BB_UFFD=1"
  # no object motion vectors (about 0.3 us less per packet on stage B; TAA may ghost)
  [nomotion]="BB_GC_BUDGET_MB=5000 BB_OBJECT_MOTION=0"
  # stage B time per part of a draw (BB_SECTIONS adds rdtsc calls: a diagnostic, not an A/B phase)
  [sections]="BB_GC_BUDGET_MB=5000 BB_SECTIONS=1"
)

for name in "${names[@]}"; do
    echo "=== $name: ${cfg[$name]:-defaults} (${secs}s) ==="
    # shellcheck disable=SC2086
    start=$SECONDS
    timeout --signal=TERM "$secs" bash tools/play_ab.sh "$name" ${cfg[$name]} || true
    if (( SECONDS - start < 20 )); then
        echo "!! the game exited after $((SECONDS - start))s, last log lines:" >&2
        tail -n 15 "$(ls -t out/ab_"$name"_*.log | head -1)" >&2
        exit 1
    fi
    pkill -x bb-probe 2>/dev/null || true   # timeout only stops the wrapper, not the game
    sleep 6
done

base_csv=$(ls -t out/ab_base_*.frames.csv 2>/dev/null | head -1)
[[ -n $base_csv ]] || { echo "No frame statistics were written; see out/ab_*.log" >&2; exit 1; }
for name in "${names[@]:1}"; do
    csv=$(ls -t out/ab_"$name"_*.frames.csv 2>/dev/null | head -1) || continue
    [[ -n $csv ]] || continue
    echo; echo "##### $name (${cfg[$name]}) vs base"
    python3 tools/frame_summary.py "$csv" --skip "$skip" --compare "$base_csv"
done

if [[ " ${names[*]} " == *" sections "* ]]; then
    log=$(ls -t out/ab_sections_*.log 2>/dev/null | head -1)
    if [[ -n $log ]]; then
        echo; echo "##### sections: stage B per packet, last 3 windows of $log"
        grep -A1 "stage B us per packet" "$log" | grep -v '^--' | tail -n 6
        echo "(share of 'packet': ObtainBuffer, buffers, vertex, index, stream copy, VRAM path)"
    fi
fi
