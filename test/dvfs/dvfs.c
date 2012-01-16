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

	unsigned long long freq = SimGetOwnFreqMHz();
	printf("Current Freq = %lld MHz\n", freq);

	freq = 5000;
	printf("Setting frequency to %lld MHz\n", freq);
	SimSetOwnFreqMHz(freq);
	freq = SimGetOwnFreqMHz();
	printf("Current Freq = %lld MHz\n", freq);

	value = 0;
	loop();

	freq = 1000;
	printf("Setting frequency to %lld MHz\n", freq);
	SimSetFreqMHz(1, freq);
	freq = SimGetFreqMHz(1);
	printf("Current Freq = %lld MHz\n", freq);

	value = 0;
	loop();

	SimRoiEnd();
}
