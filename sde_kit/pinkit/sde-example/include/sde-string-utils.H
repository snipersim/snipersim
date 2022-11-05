// 
// Copyright (C) 2021-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_STRING_UTILS_H_)
# define _SDE_STRING_UTILS_H_

#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <ctype.h>
#include "sde-c-base-types.h"
#include "sde-vreg.h"

std::string sde_tolower(const std::string& orig);

//break a string, separator is whitespace
void sde_split_string(const std::string& str, std::vector<std::string>& res);
void sde_split_string_delim(const std::string& str, char delim, std::vector<std::string>& vec);

sde_uint64_t sde_string_to_dec(const std::string& str, sde_bool_t& found);
sde_uint64_t sde_string_to_hex(const std::string& str, sde_bool_t& found,
                               sde_bool_t force_prefix = false);
sde_uint64_t sde_string_parse_num(const std::string &str);
std::string addr_to_string (sde_addr_t addr);
std::string split_vreg_string(const std::string& input, 
                             std::vector<std::string>& out, 
                             sde_uint32_t bytes);
std::string string_to_vreg(const std::string& str, sde_vreg_t& zmm, 
                           sde_uint32_t bytes);
sde_bool_t endswith(const std::string& str, const std::string& suffix);

//break the string (path) to basename
std::string sde_string_basename(const std::string &name);

std::string sde_num_grouping(const sde_uint64_t num);

#endif






