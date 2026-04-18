TARGET=sparse_writer
TARGET2=combined_attack_victim
include ../shared/Makefile.shared

# Force a clean static build to avoid RELR section issues in Pin
LDFLAGS_STATIC = -static -L$(SIM_ROOT)/lib -pthread

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(LDFLAGS_STATIC) -o $(TARGET)

$(TARGET2): $(TARGET2).o
	$(CC) $(TARGET2).o $(LDFLAGS_STATIC) -lpthread -o $(TARGET2)

all: $(TARGET) $(TARGET2)

run_attacker_alone: $(TARGET)
	mkdir -p attacker_only_output
	../../run-sniper -v -n 32 -c zen4_vn --roi -d attacker_only_output -- ./$(TARGET)

clean_extra:
	rm -rf multiproc_toleo_output multiproc_baseline_output
