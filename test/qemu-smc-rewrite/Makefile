# test/smc-rewrite recorded by the QEMU front end in user mode instead of SDE.
# Needs a build with QEMU=ON; testing/test_apps.py skips it otherwise.  The
# plugin names its file <prefix>.app0.th0.sift, where the Pin and DynamoRIO
# front ends write <prefix>.sift when there are no response files.
TARGET=smc_rewrite
FRONTEND_ARGS=--frontend=qemu
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
	  $(SNIPER_BUILD_DIR)/lib/siftdump $$(ls trace*.sift | head -1) 2>/dev/null | grep -i "^0*$$pc " | sed "s/^0*\([0-9a-f]*\) */traced \1 /"
