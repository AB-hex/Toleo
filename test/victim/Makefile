TARGET=sequential_reader
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

# Run A: victim alone under Toleo (baseline — no attacker)
run_victim_baseline:
	mkdir -p victim_baseline_output
	../../run-sniper -v -n 32 -c zen4_vn --roi -d victim_baseline_output -- ./$(TARGET)

# Run B: victim alone without Toleo (sanity check)
run_victim_no_toleo:
	mkdir -p victim_no_toleo_output
	../../run-sniper -v -n 32 -c zen4_cxl --roi -d victim_no_toleo_output -- ./$(TARGET)

run_$(TARGET): run_victim_baseline run_victim_no_toleo
	@echo ""
	@echo "=== Key metrics to compare ==="
	@echo "  victim_baseline_output/sim.out : cxl[i].total-effective-read-latency (Toleo, no attack)"
	@echo "  victim_no_toleo_output/sim.out : same, without freshness overhead"

CLEAN_EXTRA=victim_baseline_output victim_no_toleo_output *.trace *.log
