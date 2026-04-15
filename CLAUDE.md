# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**sniper-toleo** is a fork of the [Sniper multicore simulator](https://snipersim.org) extended to evaluate **Toleo** — a memory security and freshness system for tera-scale CXL memory (ASPLOS '24). It integrates DRAMSim3 for detailed DRAM modeling.

The repo lives at `toleo_root/sniper-toleo/` and expects a sibling `toleo_root/DRAMsim3/` (symlinked as `sniper-toleo/DRAMsim3 -> ../DRAMsim3`).

## Build

```bash
make USE_PIN=1 -j$(nproc)   # full build with Pin frontend
make clean                   # remove compiled objects
make distclean               # remove all auto-downloaded dependencies too
```

**Requirements:** Python 2 must be the default `python` (not Python 3). The build auto-downloads Pin 3.22, XED, and other dependencies on first run.

System packages needed:
```bash
sudo dpkg --add-architecture i386
sudo apt-get install binutils build-essential curl git libboost-dev libbz2-dev \
  libc6:i386 libncurses5:i386 libsqlite3-dev libstdc++6:i386 python wget zlib1g-dev
```

A pre-built Docker environment is available: `cd docker && make && make run`.

## Running Simulations

```bash
./run-sniper -n 32 -c <arch> -d <output-dir> -- <benchmark-command>
./run-sniper --help
```

`<arch>` options:
- `zen4_cxl` — baseline, no memory protection
- `zen4_vn` — full Toleo (Version Numbers + encryption)
- `zen4_no_freshness` — C+I (encryption + MACs, no freshness/VN)
- `zen4_no_dramsim` — lightweight Toleo without DRAMSim3 (for page type analysis)

The simulator is configured for **32 cores**. Benchmarks with >32 threads may deadlock.

## Running Tests

```bash
cd test/fft && make
```

Runs three configurations (Toleo, C+I, no-protection) and places results in `test/fft/toleo_output/`, `test/fft/ci_output/`, `test/fft/no_protection_output/`.

## Architecture

### Key Source Directories

- **`common/core/`** — Core execution models; `core.h`/`core.cc` is the main execution unit
- **`common/core/memory_subsystem/`** — All memory hierarchy code:
  - `cache/` — L1/L2 with LRU/SRRIP/MRU replacement policies
  - `dram/` — DRAM controller + DRAMSim3 integration
  - `cxl/` — CXL memory expander controllers (the main Toleo additions):
    - `cxl_cntlr.{h,cc}` — CXL expander controller
    - `cxl_vnserver_cntlr.{h,cc}` — VN server for freshness guarantees
    - `cxl_address_translator.{h,cc}` — virtual→physical with page tables
  - `mee/` — Memory Encryption Engine: `mee_naive.{h,cc}` (AES encryption + MAC + VN verification)
  - `directory_schemes/` — Cache coherence (MSI)
- **`common/performance_model/`** — Latency/bandwidth/contention models; `cxl_perf_model.h`, `vnserver/mee_perf_model.h`, `vnserver/vv_perf_model_1step.h`
- **`common/system/`** — Simulator orchestration: `simulator.h`, `core_manager.h`
- **`config/`** — Architecture config files (`zen4_cxl.cfg`, `zen4_vn.cfg`, `zen4_no_freshness.cfg`, `base.cfg`, `zen4_s.cfg` as shared base)
- **`sift/`** — SIFT compressed instruction trace format (writer/reader/library)
- **`scripts/`** — 87+ Python 2 analysis scripts for post-processing simulation output
- **`frontend/pin-frontend/`** — Pin Tool instrumentation frontend
- **`test/`** — Benchmark test scenarios (fft, mpi, fork, signal, etc.)

### Toleo-Specific Data Flow

Memory accesses flow: Core → L1/L2 cache → CXL controller → MEE (encrypt/decrypt + MAC check) → VN server (freshness verification via Version Numbers) → DRAMSim3 DRAM models.

The three configurations differ only in which MEE/VN components are active — the CXL controller selects behavior based on the `.cfg` arch file.

### Build Outputs

- `lib/libcarbon_sim.a` — Core simulator static library
- `lib/sniper` — Standalone simulator binary
- `frontend/pin-frontend/obj-intel64/pin_frontend` — Pin instrumentation tool
- `sift/libsift.a` — SIFT trace library
- `DRAMsim3/libdramsim3.so` — DRAMSim3 shared library

### Simulation Output

Each run produces in `<output-dir>/`:
- `sim.out` — human-readable statistics
- `results.json` — machine-readable stats
- `page_table.log` — page table events
- `dram_trace_analysis.csv` — per-controller DRAM access breakdown
