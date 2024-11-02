#include "hooks_py.h"
#include "simulator.h"
#include "magic_server.h"
#include "sim_api.h"

static PyObject *
setROI(PyObject *self, PyObject *args)
{
   bool inRoi = false;

   if (!PyArg_ParseTuple(args, "b", &inRoi))
      return NULL;

   Sim()->getMagicServer()->setPerformance(inRoi);

   Py_RETURN_NONE;
}

static PyObject *
setInstrumentationMode(PyObject *self, PyObject *args)
{
   long int mode = 999;

   if (!PyArg_ParseTuple(args, "l", &mode))
      return NULL;

   switch (mode)
   {
      case SIM_OPT_INSTRUMENT_DETAILED:
      case SIM_OPT_INSTRUMENT_WARMUP:
      case SIM_OPT_INSTRUMENT_FASTFORWARD:
         Sim()->getMagicServer()->Magic_unlocked(INVALID_CORE_ID, INVALID_THREAD_ID, SIM_CMD_INSTRUMENT_MODE, mode, 0);
         break;
      default:
         LOG_PRINT_ERROR("Unexpected instrumentation mode from python: %lx.", mode);
         return NULL;
   }

   Py_RETURN_NONE;
}

static PyObject *
setProgress(PyObject *self, PyObject *args)
{
   float progress = 0;

   if (!PyArg_ParseTuple(args, "f", &progress))
      return NULL;

   Sim()->getMagicServer()->setProgress(progress);

   Py_RETURN_NONE;
}

#include "trace_manager.h"
static PyObject *
simulatorAbort(PyObject *self, PyObject *args)
{
   //cannot Abort here, because we have the GIL, and it is needed to release the python interpreter, so set a flag instead, and wait once we come back from the callback.
   HooksPy::prepare_abort();
   Py_RETURN_NONE;
}

static PyMethodDef PyControlMethods[] = {
   { "set_roi", setROI, METH_VARARGS, "Set whether or not we are in the ROI" },
   { "set_instrumentation_mode", setInstrumentationMode, METH_VARARGS, "Set instrumentation mode" },
   { "set_progress", setProgress, METH_VARARGS, "Set simulation progress indicator (0..1)" },
   { "abort", simulatorAbort, METH_VARARGS, "Stop simulation now" },
   { NULL, NULL, 0, NULL } /* Sentinel */
};

static PyModuleDef PyControlModule = {
	PyModuleDef_HEAD_INIT,
	"sim_control",
	"",
	-1,
	PyControlMethods,
	NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_sim_control(void)
{
   PyObject *pModule = PyModule_Create(&PyControlModule);

   {
      PyObject *pGlobalConst = PyLong_FromLong(SIM_OPT_INSTRUMENT_DETAILED);
      PyObject_SetAttrString(pModule, "DETAILED", pGlobalConst);
      Py_DECREF(pGlobalConst);
   }
   {
      PyObject *pGlobalConst = PyLong_FromLong(SIM_OPT_INSTRUMENT_WARMUP);
      PyObject_SetAttrString(pModule, "WARMUP", pGlobalConst);
      Py_DECREF(pGlobalConst);
   }
   {
      PyObject *pGlobalConst = PyLong_FromLong(SIM_OPT_INSTRUMENT_FASTFORWARD);
      PyObject_SetAttrString(pModule, "FASTFORWARD", pGlobalConst);
      Py_DECREF(pGlobalConst);
   }
   return pModule;
}
