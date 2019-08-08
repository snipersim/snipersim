/*******************************************************************************/
/* PAPI                                                                        */
/* ====                                                                        */
/* In software that use the PAPI interface, when it's simulated over Sniper,   */
/* it obtains real HWC.                                                        */
/*                                                                             */
/* Sniper provide some hardware counters and to take profit of these with      */
/* PAPI, we need to intercept papi calls and return the desired info.          */
/*                                                                             */
/* The strategy follows here was intercept all functions on papi lib and       */
/* return PAPI_OK for all of them except some specific functions. Also,        */
/* return the hwc value.                                                       */
/*                                                                             */
/* The functionality has been tested only with Extrae with this conf. for HWC: */
/*                                                                             */
/*  <counters enabled="yes">                                                   */
/*    <cpu enabled="yes" starting-set-distribution="1">                        */
/*      <set enabled="yes" domain="all" changeat-time="0">                     */
/*        PAPI_TOT_INS,PAPI_TOT_CYC,PAPI_L1_DCM,PAPI_L2_DCM,PAPI_L3_TCM,       */
/*                PAPI_BR_MSP                                                  */
/*      </set>                                                                 */
/*    </cpu>                                                                   */
/*    <network enabled="no" />                                                 */
/*    <resource-usage enabled="no" />                                          */
/*    <memory-usage enabled="no" />                                            */
/*  </counters>                                                                */
/*                                                                             */
/*******************************************************************************/

#include "emulation.h"
#include "recorder_control.h"
#include "sift_assert.h"
#include "globals.h"
#include "threads.h"

#include <pin.H>

#include <iostream>
#include <string>

#include <iostream>
#include <string.h>

#define PAPI_OK 0

#define PAPI_TOT_INS_CODE 50
#define PAPI_TOT_CYC_CODE 59
#define PAPI_L1_DCM_CODE 0
#define PAPI_L2_DCM_CODE 2
#define PAPI_L3_TCM_CODE 8
#define PAPI_BR_MSP_CODE 46

#define PAPI_HUGE_STR_LEN     1024
#define PAPI_MIN_STR_LEN        64
#define PAPI_MAX_STR_LEN       128
#define PAPI_2MAX_STR_LEN      256

#define PAPI_MAX_INFO_TERMS  12

/*
    Funciones que son llamadas por extrae
    -------------------------------------
    PAPI_start
    PAPI_read
    PAPI_event_name_to_code
    PAPI_get_event_info
    PAPI_event_code_to_name
    PAPI_overflow
    PAPI_sampling_handler
    PAPI_stop
    PAPI_is_initialized
    PAPI_state
    PAPI_cleanup_eventset
    PAPI_destroy_eventset
    PAPI_shutdown
    PAPI_library_init
    PAPI_strerror
    PAPI_thread_init
    PAPI_create_eventset
    PAPI_add_event
    PAPI_set_opt
    PAPI_reset
    PAPI_accum
*/

typedef struct event_info {
	unsigned int event_code;
	char symbol[PAPI_HUGE_STR_LEN];
	char short_descr[PAPI_MIN_STR_LEN];
	char long_descr[PAPI_HUGE_STR_LEN];
	int component_index;
	char units[PAPI_MIN_STR_LEN];
	int location;
	int data_type;
	int value_type;
	int timescope;
	int update_type;
	int update_freq;

	/* PRESET SPECIFIC FIELDS FOLLOW */

	unsigned int count;
	unsigned int event_type;
  	char derived[PAPI_MIN_STR_LEN];
  	char postfix[PAPI_2MAX_STR_LEN];
  	unsigned int code[PAPI_MAX_INFO_TERMS];
  	char name[PAPI_MAX_INFO_TERMS]
                 [PAPI_2MAX_STR_LEN];
  	char note[PAPI_HUGE_STR_LEN];
} PAPI_event_info_t;

// Analysis routines

ADDRINT emuPAPI_start(THREADID threadid, int EventSet)
{
    if (!thread_data[threadid].output)
      return 0;

    Sift::EmuRequest req;
    Sift::EmuReply res;
    req.papi.eventset = EventSet;
    __attribute__((unused)) bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypePAPIstart, req, res);
    sift_assert(emulated);

    return PAPI_OK;
}

ADDRINT emuPAPI_read(THREADID threadid, int EventSet, long long* values)
{
    if (!thread_data[threadid].output)
      return 0;

    Sift::EmuRequest req;
    Sift::EmuReply res;
    req.papi.eventset = EventSet;
    __attribute__((unused)) bool emulated = thread_data[threadid].output->Emulate(Sift::EmuTypePAPIread, req, res);
    sift_assert(emulated);

    for(unsigned int i = 0; i < NUM_PAPI_COUNTERS; i++)
        values[i] = res.papi.values[i];
    //values = res.papi.values;

    return PAPI_OK;
}

ADDRINT emuPAPI_OK(THREADID threadid)
{
    return PAPI_OK;
}

ADDRINT emuPAPI_event_code_to_name(int EventCode, char * EventName)
{
  switch(EventCode)
  {
    case PAPI_TOT_INS_CODE:
      strcpy(EventName,"emuPAPI_TOT_INS"); break;
    case PAPI_TOT_CYC_CODE:
      strcpy(EventName,"emuPAPI_TOT_CYC"); break;
    case PAPI_L1_DCM_CODE:
      strcpy(EventName,"emuPAPI_L1_DCM"); break;
    case PAPI_L2_DCM_CODE:
      strcpy(EventName,"emuPAPI_L2_DCM"); break;
    case PAPI_L3_TCM_CODE:
      strcpy(EventName,"emuPAPI_L3_TCM"); break;
    case PAPI_BR_MSP_CODE:
      strcpy(EventName,"emuPAPI_BR_MSP"); break;
    default:
      strcpy(EventName,"emuNOT_COUNTER"); break;
  }

  return PAPI_OK;
}

ADDRINT emuPAPI_event_name_to_code(char * EventName, int * EventCode)
{

  if (!strcmp(EventName,"PAPI_TOT_INS")) *EventCode = PAPI_TOT_INS_CODE;
  else if (!strcmp(EventName,"PAPI_TOT_CYC")) *EventCode = PAPI_TOT_CYC_CODE;
  else if (!strcmp(EventName,"PAPI_L1_DCM")) *EventCode = PAPI_L1_DCM_CODE;
  else if (!strcmp(EventName,"PAPI_L2_DCM")) *EventCode = PAPI_L2_DCM_CODE;
  else if (!strcmp(EventName,"PAPI_L3_TCM")) *EventCode = PAPI_L3_TCM_CODE;
  else if (!strcmp(EventName,"PAPI_BR_MSP")) *EventCode = PAPI_BR_MSP_CODE;

  return PAPI_OK;
}

ADDRINT emuPAPI_get_event_info(int EventCode, VOID *info)
{
   PAPI_event_info_t * my_info = (PAPI_event_info_t *)info;

   my_info->count = 1;
   emuPAPI_event_code_to_name(EventCode, my_info->symbol);
   my_info->event_code = EventCode;
   strcpy(my_info->short_descr,"Simulated by Sniper");
   strcpy(my_info->long_descr,"Simulated by Sniper");

   return PAPI_OK;
}

ADDRINT emuPAPI_library_init(THREADID, int version)
{
    return version;
}

// Instrumentation routines

void imgCallback(IMG img, VOID *v)
{

    if (IMG_Name(img).find("libpapi") == std::string::npos)
        return;

    for (SEC s=IMG_SecHead(img); SEC_Valid(s); s=SEC_Next(s)) {
        for (RTN rtn=SEC_RtnHead(s); RTN_Valid(rtn); rtn=RTN_Next(rtn)) {
            std::string routine_name = RTN_Name(rtn);

            if (routine_name.find("PAPI_") != std::string::npos) {
                if (routine_name.find("PAPI_start") != std::string::npos)
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_start), IARG_THREAD_ID, IARG_END);
                else if (routine_name.find("PAPI_read") != std::string::npos)
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_read), IARG_THREAD_ID,
                        IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
                else if (routine_name.find("PAPI_event_name_to_code") != std::string::npos)
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_event_name_to_code),
                        IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
                else if (routine_name.find("PAPI_event_code_to_name") != std::string::npos)
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_event_code_to_name),
                        IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
                else if (routine_name.find("PAPI_library_init") != std::string::npos)
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_library_init),
                        IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,IARG_END);
                else if (routine_name.find("PAPI_get_event_info") != std::string::npos)
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_get_event_info),
                    IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
                else
                    RTN_ReplaceSignature(rtn, AFUNPTR(emuPAPI_OK), IARG_THREAD_ID, IARG_END);
}}}}

void initPapiInterceptors()
{
    IMG_AddInstrumentFunction(imgCallback, 0);
}
