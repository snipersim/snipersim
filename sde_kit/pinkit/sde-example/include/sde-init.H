// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_INIT_H_)
# define _SDE_INIT_H_
// Include this file in your pin tool to add an SDE emulator. Then call
// sde_init() after PinInit() in your tool to activate then emulator.
void sde_init();

/// Call this instead of PIN_Init()
void sde_pin_init(int argc, char** argv);

namespace INSTLIB {

// The function sde_init() must call sde_internal_init()
void sde_internal_init();

} // namespace


#endif
