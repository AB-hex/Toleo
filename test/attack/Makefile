TARGET=sparse_writer
TARGET2=combined_attack_victim
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

$(TARGET2): $(TARGET2).o
	$(CC) $(TARGET2).o $(SNIPER_LDFLAGS) -lpthread -o $(TARGET2)

all: $(TARGET) $(TARGET2)

# Run A: attacker alone — confirms OVERFLOW saturation
run_attacker_alone: $(TARGET)
	mkdir -p attacker_only_output
	../../run-sniper -v -n 32 -c zen4_vn --roi -d attacker_only_output -- ./$(TARGET)

# Run B: combined attacker+victim — demonstrates DoS (compare victim latency)
run_combined_toleo: $(TARGET2)
	mkdir -p combined_toleo_output
	../../run-sniper -v -n 32 -c zen4_vn --roi -d combined_toleo_output -- ./$(TARGET2) 8

# Run C: combined without Toleo — shows victim is unaffected without shared VN resource
run_combined_baseline: $(TARGET2)
	mkdir -p combined_baseline_output
	../../run-sniper -v -n 32 -c zen4_cxl --roi -d combined_baseline_output -- ./$(TARGET2) 8

run_$(TARGET): run_attacker_alone run_combined_toleo run_combined_baseline
	@echo ""
	@echo "=== Key metrics to compare ==="
	@echo "  attacker_only_output/sim.out   : vv[0].one-step-to-vault, vv[0].dram-writes"
	@echo "  combined_toleo_output/sim.out  : cxl[i].total-effective-read-latency, mee[i].vn-evictions"
	@echo "  combined_baseline_output/sim.out: same, but with no Toleo overhead (baseline)"

CLEAN_EXTRA=attacker_only_output combined_toleo_output combined_baseline_output *.trace *.log $(TARGET2) $(TARGET2).o
