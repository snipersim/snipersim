#include "hooks_py.h"
#include "subsecond_time.h"
#include "simulator.h"
#include "hooks_manager.h"
#include "magic_server.h"
#include "syscall_model.h"
#include "sim_api.h"

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

static SInt64 hookCallbackNone(UInt64 pFunc, UInt64)
{
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, NULL);
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackInt(UInt64 pFunc, UInt64 argument)
{
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(L)", argument));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackSubsecondTime(UInt64 pFunc, UInt64 argument)
{
   SubsecondTime time(*(subsecond_time_t*)&argument);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(L)", time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackString(UInt64 pFunc, UInt64 _argument)
{
   const char* argument = (const char*)_argument;
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(s)", argument));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackMagicMarkerType(UInt64 pFunc, UInt64 _argument)
{
   MagicServer::MagicMarkerType* argument = (MagicServer::MagicMarkerType*)_argument;
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(iiKKs)", argument->thread_id, argument->core_id, argument->arg0, argument->arg1, argument->str));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadCreateType(UInt64 pFunc, UInt64 _argument)
{
   HooksManager::ThreadCreate* argument = (HooksManager::ThreadCreate*)_argument;
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(ii)", argument->thread_id, argument->creator_thread_id));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadTimeType(UInt64 pFunc, UInt64 _argument)
{
   HooksManager::ThreadTime* argument = (HooksManager::ThreadTime*)_argument;
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(iL)", argument->thread_id, time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadStallType(UInt64 pFunc, UInt64 _argument)
{
   HooksManager::ThreadStall* argument = (HooksManager::ThreadStall*)_argument;
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(isL)", argument->thread_id, ThreadManager::stall_type_names[argument->reason], time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadResumeType(UInt64 pFunc, UInt64 _argument)
{
   HooksManager::ThreadResume* argument = (HooksManager::ThreadResume*)_argument;
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(iiL)", argument->thread_id, argument->thread_by, time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackThreadMigrateType(UInt64 pFunc, UInt64 _argument)
{
   HooksManager::ThreadMigrate* argument = (HooksManager::ThreadMigrate*)_argument;
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(iiL)", argument->thread_id, argument->core_id, time.getFS()));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackSyscallEnter(UInt64 pFunc, UInt64 _argument)
{
   SyscallMdl::HookSyscallEnter* argument = (SyscallMdl::HookSyscallEnter*)_argument;
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(iiLi(llllll))", argument->thread_id, argument->core_id, time.getFS(),
      argument->syscall_number, argument->args.arg0, argument->args.arg1, argument->args.arg2, argument->args.arg3, argument->args.arg4, argument->args.arg5));
   return hookCallbackResult(pResult);
}

static SInt64 hookCallbackSyscallExit(UInt64 pFunc, UInt64 _argument)
{
   SyscallMdl::HookSyscallExit* argument = (SyscallMdl::HookSyscallExit*)_argument;
   SubsecondTime time(argument->time);
   PyObject *pResult = HooksPy::callPythonFunction((PyObject *)pFunc, Py_BuildValue("(iiLiO)", argument->thread_id, argument->core_id, time.getFS(),
      argument->ret_val, argument->emulated ? Py_True : Py_False));
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
         Sim()->getHooksManager()->registerHook(type, hookCallbackSubsecondTime, (UInt64)pFunc);
         break;
      case HookType::HOOK_SIM_START:
      case HookType::HOOK_SIM_END:
      case HookType::HOOK_ROI_BEGIN:
      case HookType::HOOK_ROI_END:
      case HookType::HOOK_APPLICATION_ROI_BEGIN:
      case HookType::HOOK_APPLICATION_ROI_END:
      case HookType::HOOK_SIGUSR1:
         Sim()->getHooksManager()->registerHook(type, hookCallbackNone, (UInt64)pFunc);
         break;
      case HookType::HOOK_PERIODIC_INS:
      case HookType::HOOK_CPUFREQ_CHANGE:
      case HookType::HOOK_INSTR_COUNT:
      case HookType::HOOK_INSTRUMENT_MODE:
      case HookType::HOOK_APPLICATION_START:
      case HookType::HOOK_APPLICATION_EXIT:
         Sim()->getHooksManager()->registerHook(type, hookCallbackInt, (UInt64)pFunc);
         break;
      case HookType::HOOK_PRE_STAT_WRITE:
         Sim()->getHooksManager()->registerHook(type, hookCallbackString, (UInt64)pFunc);
         break;
      case HookType::HOOK_MAGIC_MARKER:
      case HookType::HOOK_MAGIC_USER:
         Sim()->getHooksManager()->registerHook(type, hookCallbackMagicMarkerType, (UInt64)pFunc);
         break;
      case HookType::HOOK_THREAD_CREATE:
         Sim()->getHooksManager()->registerHook(type, hookCallbackThreadCreateType, (UInt64)pFunc);
         break;
      case HookType::HOOK_THREAD_START:
      case HookType::HOOK_THREAD_EXIT:
         Sim()->getHooksManager()->registerHook(type, hookCallbackThreadTimeType, (UInt64)pFunc);
         break;
      case HookType::HOOK_THREAD_STALL:
         Sim()->getHooksManager()->registerHook(type, hookCallbackThreadStallType, (UInt64)pFunc);
         break;
      case HookType::HOOK_THREAD_RESUME:
         Sim()->getHooksManager()->registerHook(type, hookCallbackThreadResumeType, (UInt64)pFunc);
         break;
      case HookType::HOOK_THREAD_MIGRATE:
         Sim()->getHooksManager()->registerHook(type, hookCallbackThreadMigrateType, (UInt64)pFunc);
         break;
      case HookType::HOOK_SYSCALL_ENTER:
         Sim()->getHooksManager()->registerHook(type, hookCallbackSyscallEnter, (UInt64)pFunc);
         break;
      case HookType::HOOK_SYSCALL_EXIT:
         Sim()->getHooksManager()->registerHook(type, hookCallbackSyscallExit, (UInt64)pFunc);
         break;
      case HookType::HOOK_TYPES_MAX:
         assert(0);
   }

   Py_RETURN_NONE;
}

static PyObject *
triggerHookMagicUser(PyObject *self, PyObject *args)
{
   UInt64 a, b;

   if (!PyArg_ParseTuple(args, "ll", &a, &b))
      return NULL;

   UInt64 res = Sim()->getMagicServer()->Magic_unlocked(INVALID_THREAD_ID, INVALID_CORE_ID, SIM_CMD_USER, a, b);

   return PyInt_FromLong(res);
}

static PyMethodDef PyHooksMethods[] = {
   {"register",  registerHook, METH_VARARGS, "Register callback function to a Sniper hook."},
   {"trigger_magic_user", triggerHookMagicUser, METH_VARARGS, "Trigger HOOK_MAGIC_USER hook."},
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
