# run-sniper --pa on a host that cannot read physical frame numbers (no
# CAP_SYS_ADMIN): it must refuse with the reason, not quietly simulate
# without physical addresses.  testing/test_apps.py runs this case only on
# such a host; test/pa covers the other.
TARGET=pa
CLEAN_EXTRA=pa.c
include ../shared/Makefile.shared

pa.c: ../pa/pa.c
	cp $< $@

$(TARGET): $(TARGET).o
	$(CC) $(TARGET).o $(SNIPER_LDFLAGS) -o $(TARGET)

run_$(TARGET):
	@../../run-sniper -n 1 -c gainestown --pa -- ./$(TARGET); echo "pa-refused exit=$$?"
