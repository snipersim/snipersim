#!/bin/bash


EXECUTABLE=./run-sniper 
CONFIG=./config/virtuoso_configs/virtuoso_reservethp_detailed_mmu.cfg
SIFT=./traces/randacc.sift # Example SIFT trace file: Replace with your own trace file as needed
OUTPUT_DIR=./test_detailed_mmu

mkdir -p $OUTPUT_DIR

$EXECUTABLE  -c $CONFIG -d $OUTPUT_DIR -s stop-by-icount:3000000 --traces=$SIFT