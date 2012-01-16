#!/bin/bash

cd ~/redmine-git/graphite
export BENCHMARKS_ROOT=~/redmine-git/benchmarks
(
	./tools/regressiontests.py $1

	#ssh atuin iqvsub -N 10 -c haunter -q short
	#ssh atuin iqvsub -N 10 -c gengar -q long
) 2>&1 1>regressiontests.log
