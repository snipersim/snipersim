#include "hooks_py.h"
#include "subsecond_time.h"
#include "simulator.h"
#include "hooks_manager.h"
#include "magic_server.h"

static SInt64 hookCallbackResult(PyObject *pResult)
{
   SInt64 result = -1;
   if (pResult == NULL)
      return -1;
   else if (PyInt_Check(pResult))
      result = PyInt_AsLong(pResult);
   else if (PyLong_Check(pResult))
      result = PyLong_AsLong(pResult);
   Py_DECREF(pResult);
   return result;
}

static SInt64 hookCallbackNone(PyObject *pFunc, void*)
{
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, NULL);
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackInt(PyObject *pFunc, SInt64 argument)
{
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(L)", argument));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackSubsecondTime(PyObject *pFunc, subsecond_time_t argument)
{
   SubsecondTime time(argument);
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(L)", time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackMagicMarkerType(PyObject *pFunc, MagicServer::MagicMarkerType* argument)
{
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(lll)", argument->core_id, argument->arg0, argument->arg1));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadStallType(PyObject *pFunc, HooksManager::ThreadStall* argument)
{
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(lL)", argument->core_id, time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadResumeType(PyObject *pFunc, HooksManager::ThreadResume* argument)
{
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(llL)", argument->core_id, argument->core_by, time.getFS()));
   return hookCallbackResult(pResult);
}

static PyObject *
registerHook(PyObject *self, PyObject *args)
{
   int hook = -1;
   PyObject *pFunc = NULL;

   if (!PyArg_ParseTuple(args, "lO", &hook, &pFunc))
      return NULL;

   if (hook < 0 || hook >= HookType::HOOK_TYPES_MAX) {
      PyErr_SetString(PyExc_ValueError, "Hook type out of range");
      return NULL;
   }
   if (!PyCallable_Check(pFunc)) {
      PyErr_SetString(PyExc_TypeError, "Second argument must be callable");
      return NULL;
   }

   Py_INCREF(pFunc);

   HookType::hook_type_t type = HookType::hook_type_t(hook);
   switch(type) {
      case HookType::HOOK_PERIODIC:
         Sim()->getHooksManager()->registerHook(type, (HooksManager::HookCallbackFunc)hookCallbackSubsecondTime, (void*)pFunc);
         break;
      case HookType::HOOK_SIM_START:
      case HookType::HOOK_SIM_END:
      case HookType::HOOK_ROI_BEGIN:
      case HookType::HOOK_ROI_END:
         Sim()->getHooksManager()->registerHook(type, (HooksManager::HookCallbackFunc)hookCallbackNone, (void*)pFunc);
         break;
      case HookType::HOOK_CPUFREQ_CHANGE:
      case HookType::HOOK_INSTR_COUNT:
      case HookType::HOOK_INSTRUMENT_MODE:
         Sim()->getHooksManager()->registerHook(type, (HooksManager::HookCallbackFunc)hookCallbackInt, (void*)pFunc);
         break;
      case HookType::HOOK_MAGIC_MARKER:
      case HookType::HOOK_MAGIC_USER:
         Sim()->getHooksManager()->registerHook(type, (HooksManager::HookCallbackFunc)hookCallbackMagicMarkerType, (void*)pFunc);
         break;
      case HookType::HOOK_THREAD_STALL:
         Sim()->getHooksManager()->registerHook(type, (HooksManager::HookCallbackFunc)hookCallbackThreadStallType, (void*)pFunc);
         break;
      case HookType::HOOK_THREAD_RESUME:
         Sim()->getHooksManager()->registerHook(type, (HooksManager::HookCallbackFunc)hookCallbackThreadResumeType, (void*)pFunc);
         break;
      case HookType::HOOK_TYPES_MAX:
         assert(0);
   }

   Py_RETURN_NONE;
}

static PyMethodDef PyHooksMethods[] = {
   {"register",  registerHook, METH_VARARGS, "Register callback function to a Sniper hook."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

void HooksPy::PyHooks::setup()
{
   PyObject *pModule = Py_InitModule("sim_hooks", PyHooksMethods);
   PyObject *pHooks = PyDict_New();
   PyObject_SetAttrString(pModule, "hooks", pHooks);

   for(int i = 0; i < int(HookType::HOOK_TYPES_MAX); ++i) {
      PyObject *pGlobalConst = PyInt_FromLong(i);
      PyObject_SetAttrString(pModule, HookType::hook_type_names[i], pGlobalConst);
      PyDict_SetItemString(pHooks, HookType::hook_type_names[i], pGlobalConst);
      Py_DECREF(pGlobalConst);
   }
   Py_DECREF(pHooks);
}
