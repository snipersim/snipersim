# The DynamoRIO front end where it does not keep the guest's time (an ISA
# other than x86-64; here forced by -refuse_time): the guest's first clock
# read, wait with a timeout or timer ends the run with the reason, rather
# than the host's time reaching the guest; a program that uses no time runs,
# and a counter read reads the instructions run (frontend/dr-frontend/
# dr_fe_time.cc).  The guest is testing/apps/time_uses.c.  Needs
# DYNAMORIO=ON; testing/test_apps.py skips the case otherwise.
TARGET=time_uses
include ../shared/Makefile.shared

APPS=../../testing/apps

time_uses: $(APPS)/time_uses.c
	$(CC) -O1 $(APPS)/time_uses.c -o time_uses

run_$(TARGET):
	for m in none poll0 rdtsc vdso raw sleep poll; do \
	  ../../run-sniper --frontend=dr -n 1 -c gainestown -d $${SNIPER_OUTPUT_DIR:-.}/$$m --trace-args=-refuse_time -- ./time_uses $$m; \
	  echo "time_uses: mode $$m exit $$?"; \
	done
