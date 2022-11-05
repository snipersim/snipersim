// 
// Copyright (C) 2017-2021 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#if !defined(_SDE_PINPLAY_SUPP_H_)
#define _SDE_PINPLAY_SUPP_H_

#include "pin.H"
#include "instlib.H"
#include "pinplay.H"
#include "control_manager.H"

using namespace CONTROLLER;

class ISIMPOINT;

void sde_tracing_activate(int argc, char *argv[]);
void sde_isimpoint_activate(int argc, char *argv[]);
PINPLAY_ENGINE * sde_tracing_get_pinplay_engine();
ISIMPOINT * sde_tracing_get_isimpoint();
bool sde_is_tracing_active();
bool sde_is_replayer_active();
bool sde_is_logger_active();

// Thread id translation callback and accessors
void sde_set_thread_trans_callback(THREAD_TRANS_CALLBACK thread_trans_callback);
THREAD_TRANS_CALLBACK sde_get_thread_trans_callback();
THREADID sde_get_translated_tid(THREADID tid);

#endif
