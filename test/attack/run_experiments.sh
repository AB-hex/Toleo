#!/bin/bash
# run_experiments.sh — Run all 4 experiments with random_reader victim
#
# Relaxed assumptions (vs. full simulation):
#   - 4 cores instead of 32 (only 2 used; saves simulator overhead)
#   - Constant DRAM model (45ns) instead of DRAMSim3 (bus queueing is what matters)
#   - 50K random reads (enough CXL traffic to show contention)
#
# Usage: bash run_experiments.sh
set -e

cd "$(dirname "$0")"
TRACES=/tmp/toleo_traces
CORES=4

echo "============================================"
echo "  Experiment Suite: Random Reader Victim"
echo "  Cores: $CORES  |  DRAM: constant 45ns"
echo "============================================"
echo ""

# Ensure traces exist
for f in attacker.sift random_victim.sift; do
    if [ ! -f "$TRACES/$f" ]; then
        echo "ERROR: $TRACES/$f not found"; exit 1
    fi
done

run_exp() {
    local name=$1 dir=$2 cfg=$3 traces=$4
    echo "[$name] Starting..."
    rm -rf "$dir" && mkdir -p "$dir"
    ../../run-sniper -n $CORES -c "$cfg" --sim-end=last -d "$dir" --traces="$traces" 2>&1 \
        | grep -E "Elapsed|ERROR|error" || true
    if [ -s "$dir/sim.out" ]; then
        echo "[$name] Done. IPC:"
        head -5 "$dir/sim.out" | grep "IPC"
    else
        echo "[$name] WARNING: empty sim.out"
    fi
    echo ""
}

# A'  — Victim alone, Toleo
run_exp "1/5 Run A — Victim alone (Toleo)" \
    random_victim_alone_toleo zen4_vn "$TRACES/random_victim.sift"

# A'b — Victim alone, no Toleo
run_exp "2/5 Run A'b — Victim alone (no Toleo)" \
    random_victim_alone_baseline zen4_cxl "$TRACES/random_victim.sift"

# C'  — Combined, Toleo
run_exp "3/5 Run C — Combined (Toleo)" \
    random_combined_toleo zen4_vn "$TRACES/attacker.sift,$TRACES/random_victim.sift"

# D'  — Combined, no Toleo
run_exp "4/5 Run D — Combined (no Toleo, control)" \
    random_combined_baseline zen4_cxl "$TRACES/attacker.sift,$TRACES/random_victim.sift"

# E   — Combined, Toleo + dynamic per-tenant VN throttling (mitigation)
run_exp "5/5 Run E — Combined (Toleo + throttle mitigation)" \
    random_combined_throttle zen4_vn_throttle "$TRACES/attacker.sift,$TRACES/random_victim.sift"

echo "============================================"
echo "  All done. Call me back to analyze results."
echo "============================================"
