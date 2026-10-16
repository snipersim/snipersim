# test/smc-rewrite recorded by the DynamoRIO front end instead of SDE.
# Needs a build with DYNAMORIO=ON; testing/test_apps.py skips it otherwise.
TARGET=smc_rewrite
FRONTEND_ARGS=--frontend=dr
CLEAN_EXTRA=smc_rewrite.c trace* record.log
include ../shared/Makefile.shared

smc_rewrite.c: ../smc-rewrite/smc_rewrite.c
	cp $< $@

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	../../run-sniper -n 1 -c gainestown $(FRONTEND_ARGS) -- ./$(TARGET)
	@rm -f trace*.sift
	../../record-trace $(FRONTEND_ARGS) -o trace -- ./$(TARGET) > record.log 2>&1
	@pc=$$(sed -n 's/.*B rewritten pc=0x\([0-9a-f]*\).*/\1/p' record.log); \
	  $(SNIPER_BUILD_DIR)/lib/siftdump trace.sift 2>/dev/null | grep -i "^0*$$pc " | sed "s/^0*\([0-9a-f]*\) */traced \1 /"
