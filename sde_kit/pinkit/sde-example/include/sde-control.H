// 
// Copyright (C) 2004-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#if !defined(_SDE_CONTROL_H_)
# define _SDE_CONTROL_H_

#include "control_manager.H"

#define SDE_CONTROL_NO_CONTEXT 0
#define SDE_CONTROL_PASS_CONTEXT 1
namespace SDE_CONTROLLER {
CONTROLLER::CONTROL_MANAGER* sde_controller_get();
}

#endif

