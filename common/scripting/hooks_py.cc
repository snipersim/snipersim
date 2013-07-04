#include "hooks_py.h"
#include "simulator.h"
#include "core_manager.h"
#include "config.hpp"
#include "fxsupport.h"

bool HooksPy::pyInit = false;

void HooksPy::init()
{
   UInt64 numscripts = Sim()->getCfg()->getInt("hooks/numscripts");
   for(UInt64 i = 0; i < numscripts; ++i) {
      String scriptname = Sim()->getCfg()->getString(String("hooks/script") + itostr(i) + "name");
      if (scriptname.substr(scriptname.length()-3) == ".py") {
         if (! pyInit) {
            setup();
         }

         String args = Sim()->getCfg()->getString(String("hooks/script") + itostr(i) + "args");
         char *argv[] = { (char*)(scriptname.c_str()), (char*)(args.c_str()) };
         PySys_SetArgvEx(2, argv, 0 /* updatepath */);

         printf("Executing Python script %s\n", scriptname.c_str());
         int s = PyRun_SimpleFileEx(fopen(scriptname.c_str(), "r"), scriptname.c_str(), 1 /* closeit */);
         if (s != 0) {
            PyErr_Print();
            fprintf(stderr, "Cannot open Python script %s\n", scriptname.c_str());
            exit(-1);
         }
      }
   }
}

void HooksPy::setup()
{
   pyInit = true;
   const char* sim_root = NULL;
   const char env_roots[2][16] = {"SNIPER_ROOT", "GRAPHITE_ROOT"};
   for (unsigned int i = 0 ; i < 2 ; i++)
   {
      sim_root = getenv(env_roots[i]);
      if (sim_root)
         break;
   }
   LOG_ASSERT_ERROR(sim_root, "Please make sure SNIPER_ROOT or GRAPHITE_ROOT is set");
#ifdef TARGET_INTEL64
   String python_home = String(sim_root) + "/python_kit/intel64";
#else
   String python_home = String(sim_root) + "/python_kit/ia32";
#endif
   Py_SetPythonHome(strdup(python_home.c_str()));
   Py_InitializeEx(0 /* don't initialize signal handlers */);

   // set up all components
   PyConfig::setup();
   PyStats::setup();
   PyHooks::setup();
   PyDvfs::setup();
   PyControl::setup();
   PyBbv::setup();
   PyMem::setup();
   PyThread::setup();
}

void HooksPy::fini()
{
   if (pyInit)
      Py_Finalize();
}

PyObject * HooksPy::callPythonFunction(PyObject *pFunc, PyObject *pArgs)
{
   PyObject *pResult = PyObject_CallObject(pFunc, pArgs);
   Py_XDECREF(pArgs);
   if (pResult == NULL) {
      PyErr_Print();
   }
   return pResult;
}
