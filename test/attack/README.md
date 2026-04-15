# Weaponizing Version Locality: Resource Contention Attack Against Toleo

## Overview

This directory contains benchmarks and tooling to demonstrate a Denial-of-Service
attack against the Toleo memory freshness architecture. A single unprivileged
attacker process exploits Toleo's metadata compression to generate amplified
traffic on the shared CXL VN bus, starving co-located victim workloads.

## How the Attack Works

### Toleo's compression state machine

Toleo tracks per-page version numbers in three compression states
(defined in `vv_perf_model_1step.h`):

```
ONE_STEP (16 B/page)  -->  VAULT (128 B/page)  -->  OVERFLOW (512 B/page)
       uniform VN             per-CL tracking           full per-CL
```

- **ONE_STEP -> VAULT** triggers when any cacheline's private version counter
  exceeds 1 (`vn_private[cl] > 1` in `VN_Page::update()`).
- **VAULT -> OVERFLOW** triggers when any CL reaches version 127.

### Simulator abstraction (important)

The simulator **resets** every VN\_Page back to ONE\_STEP after each state
transition (`vv_perf_model_1step.cc:168`):

```cpp
m_vault_pages[page_num] = VN_Page();  // reset to fresh ONE_STEP
```

This means **OVERFLOW is unreachable** — pages oscillate between ONE\_STEP
and VAULT. The 32-access OVERFLOW penalty loop never fires. The real
amplification comes entirely from frequent ONE\_STEP -> VAULT transitions.

### Amplification per transition

| Phase | DRAM ops | Notes |
|-------|----------|-------|
| Read VN entry | 1 | ONE\_STEP: single 16 B read |
| Normal write | 1 | Update the dirty entry |
| Transition penalty | **8** | Write 128 B VAULT entry (128 / 16 = 8 ops) |
| **Total on transition** | **10** | |
| **Total without transition** | **2** | |

**Amplification factor: 5x** per transition on the shared 3.32 GB/s CXL VN bus.

### Why the attack pattern writes CL 0 repeatedly

The attacker writes to **CL 0 of every page** in a loop. This works because:

1. **L3 conflict evictions guarantee CXL write-backs.** The L3 cache is 16 MB,
   16-way set-associative with `address_hash = mask`. At stride 4096 (one page),
   cache lines map to only 256 out of 16,384 L3 sets. With 16,384 pages, each
   set receives 64 lines but can hold only 16 — causing ~75% eviction rate per
   pass. Each eviction of a dirty CL 0 generates a CXL write-back and a
   VN\_UPDATE.

2. **Every 2 VN\_UPDATEs to the same CL triggers a transition.** First update:
   `vn_private[0] = 1` (no transition). Second update: `vn_private[0] = 2 > 1`
   -> VAULT transition -> page reset -> repeat.

### What does NOT work (and why)

Spreading writes uniformly across all 64 CLs per page (e.g., using a stride
coprime to 64) causes `m_total_offset` to reach 64, triggering the rollover
in `VN_Page::update()` which resets all counters and keeps the page in
ONE\_STEP forever. This maintains **perfect** version locality and generates
**zero** transitions. The attack requires concentrated writes to a small
number of CLs.

## Files

| File | Purpose |
|------|---------|
| `sparse_writer.c` | Standalone attacker: writes CL 0 of 16,384 pages |
| `combined_attack_victim.c` | Multi-threaded DoS demo: N/2 attackers + N/2 victims |
| `Makefile` | Build and run all experiments |
| `../victim/sequential_reader.c` | Victim baseline: sequential writes (ONE\_STEP) |
| `../victim/Makefile` | Build and run victim baselines |
| `../../config/zen4_vn_attack.cfg` | Attack config (64-entry VN table for faster pressure) |

## Building

Prerequisites: the simulator must be compiled first.

```bash
cd /path/to/sniper-toleo
make USE_PIN=1 -j$(nproc)
```

Then build the benchmarks:

```bash
cd test/attack
make all            # builds sparse_writer and combined_attack_victim

cd ../victim
make sequential_reader
```

## Running the Experiments

### Experiment matrix

| Run | Command | Purpose |
|-----|---------|---------|
| A — Victim baseline | `cd test/victim && make run_victim_baseline` | Victim alone under Toleo |
| B — Attacker alone | `cd test/attack && make run_attacker_alone` | Confirm transitions happen |
| C — Combined (Toleo) | `cd test/attack && make run_combined_toleo` | **DoS demonstration** |
| D — Combined (no Toleo) | `cd test/attack && make run_combined_baseline` | Control (no VN bus) |

Or run all attack experiments at once:

```bash
cd test/attack && make run_sparse_writer
```

### Using zen4_vn_attack.cfg (optional, more aggressive)

The `zen4_vn_attack.cfg` config reduces the VN table from 256 to 64 entries,
amplifying eviction pressure. To use it, edit the Makefile targets or run
directly:

```bash
../../run-sniper -v -n 32 -c zen4_vn_attack --roi \
    -d attack_aggressive_output -- ./sparse_writer
```

## Reading the Results

After each run, check `<output_dir>/sim.out` for these metrics:

### Attack telemetry (new stats added by this project)

```
vv[0].one-step-to-vault      # Count of ONE_STEP -> VAULT transitions
                              # Each = 8 extra DRAM writes on VN bus
                              # This is the PRIMARY attack metric

vv[0].vault-to-overflow       # Always 0 (simulator abstraction, see above)
vv[0].overflow-penalty-iters  # Always 0 (simulator abstraction, see above)

mee[N].vn-evictions           # VN table evictions (Overflow Buffer pressure proxy)
                              # Higher = more VN cache thrashing
```

### Existing stats to compare

```
vv[0].dram-writes             # Total VN DRAM writes (includes transition penalty)
                              # Compare: attacker run vs victim baseline
                              # Ratio shows metadata amplification

vv[0].dram-reads              # Total VN DRAM reads

cxl[N].total-effective-read-latency
                              # Cumulative CXL read latency
                              # Compare Run C vs Run A: degradation = DoS impact

mee[N].vn-misses              # VN table miss count
                              # Higher under attack = more CXL VN bus traffic
```

### What to look for

1. **Run B** (`attacker_only_output/sim.out`):
   - `vv[0].one-step-to-vault` should be in the thousands
   - `vv[0].dram-writes` should be much larger than `vv[0].dram-reads`
     (because each transition adds 8 writes)

2. **Run C vs Run A** (combined Toleo vs victim baseline):
   - `cxl[N].total-effective-read-latency` should be higher in Run C
   - The ratio = DoS amplification factor

3. **Run C vs Run D** (Toleo vs no-Toleo):
   - If victim degradation exists in Run C but NOT in Run D, the DoS is
     Toleo-specific (the shared VN bus is the bottleneck, not raw memory
     bandwidth)

## Simulator Code Changes

All changes are in `sniper-toleo/`:

### Modified files

**`common/performance_model/vnserver/vv_perf_model_1step.h`**
- Added `m_one_step_to_vault`, `m_vault_to_overflow`, `m_overflow_penalty_iters`
  counters to `VVPerfModel1Step` class

**`common/performance_model/vnserver/vv_perf_model_1step.cc`**
- Initialized counters in constructor, registered via `registerStatsMetric()`
- Transition detection at line 163: after `page_it->second.update(cl_num)`,
  compare `new_page_type` vs `vn_page_type` to identify ONE\_STEP->VAULT
  and VAULT->OVERFLOW transitions
- OVERFLOW penalty tracking at line 147: count read-loop iterations when
  page is in OVER\_FLOW state (never fires due to simulator abstraction,
  kept for completeness)

**`common/core/memory_subsystem/mee/mee_naive.h`**
- Added `m_vn_evictions` counter

**`common/core/memory_subsystem/mee/mee_naive.cc`**
- Initialized `m_vn_evictions` to 0 in constructor
- Registered stat: `registerStatsMetric("mee", m_core_id, "vn-evictions", ...)`
- Incremented in `insertVN()` when `insertSingleLine` reports an eviction

## Mitigation and its Contradiction

A potential mitigation is statically partitioning the VN table and VN server
bandwidth per tenant. However, this directly undermines Toleo's core value
proposition:

- Toleo claims 240x metadata compression precisely because all tenants share
  the VN infrastructure
- Partitioning caps each tenant's VN table to `256/N` entries (with N tenants)
- This forces more frequent VN misses and CXL fetches for ALL tenants, even
  benign ones
- At scale (many tenants), the per-tenant VN table becomes too small to be
  effective, and Toleo degrades to an uncompressed scheme

**The shared resource that enables Toleo's compression is the same resource
that enables the DoS.** This is the fundamental architectural contradiction.
