// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_OUTPUT_FILENAME_ADDITIONS_H_)
#define _SDE_OUTPUT_FILENAME_ADDITIONS_H_

#include <string>
#include "sde-base-types.H"

// Public function for output file name modification
// In this routine we add pid and directory to file name
// if needed according to the knobs
std::string
sde_modify_output_file_name(std::string filename, sde_uint32_t tid);

std::ofstream*
sde_open_standard_output_file_name_stream(std::string filename);

FILE*
sde_open_standard_output_file_name_file(std::string filename);

void init_standard_output_child_process_support();

#endif
