TARGET=chase
CLEAN_EXTRA=chase.sift
include ../shared/Makefile.shared

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

# The chase alone, then as two applications three times over: which
# application's fault comes first varies from run to run, and the old
# allocator's result varied with it
OUT=$(or $(SNIPER_OUTPUT_DIR),.)
run_$(TARGET):
	../../record-trace --roi -o chase -- ./chase
	../../run-sniper -n 1 -c gainestown -d $(OUT)/one --traces=chase.sift
	for i in 1 2 3; do ../../run-sniper -n 2 -c gainestown -d $(OUT)/two-$$i --traces=chase.sift,chase.sift || exit 1; done
