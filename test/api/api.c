#include "sim_api.h"

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#include <stdint.h>
#include <stdio.h>

int value;

int loop() {

	int i;
	for (i = 0 ; i < 10000; i++) {
		value += i;
	}
	return value;
}

int main() {

	SimRoiStart();

	value = 0;
	loop();

	if (SimInSimulator()) {
		printf("API Test: Running in the simulator\n"); fflush(stdout);
	} else {
		printf("API Test: Not running in the simulator\n"); fflush(stdout);
	}

	SimRoiEnd();
}
