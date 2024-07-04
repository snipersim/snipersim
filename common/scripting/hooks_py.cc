#include "hooks_py.h"
#include "simulator.h"
#include "config.hpp"

bool HooksPy::pyInit = false;

void HooksPy::init()
{
   //NOTE this modification to python3 is only tested with numscripts=1
   UInt64 numscripts = Sim()->getCfg()->getInt("hooks/numscripts");
   for(UInt64 i = 0; i < numscripts; ++i) {
      String scriptname = Sim()->getCfg()->getString(String("hooks/script") + itostr(i) + "name");
      if (scriptname.substr(scriptname.length()-3) == ".py") {
	 std::string sim_root = get_root();
	 set_env();

#if defined(TARGET_INTEL64) || defined(TARGET_IA32E)
	 std::string python_home = sim_root + "/python_kit/intel64/bin/python";
#elif defined(TARGET_IA32)
	 std::string python_home = sim_root + "/python_kit/ia32/bin/python";
#else
	 #error "Unknown TARGET definition"
#endif

	 PyStatus status;
	 PyConfig config;
	 PyConfig_InitIsolatedConfig(&config);
	 
	 //This automatically finds pyenv.cfg in directory above (bin) to load the virtual environment
	 status = PyConfig_SetString(&config, &config.executable, std::wstring(python_home.begin(), python_home.end()).c_str());
	 if (PyStatus_Exception(status)){
	 	fprintf(stderr, "Cannot set home string of Python config");
	 	exit(-1);
	 }

	 //This code is not necessary, as run-sniper takes care of setting the path correctly
	 //std::string scripts_path = sim_root + "/scripts";
	 //PyConfig_SetString(&config, &config.pythonpath_env, std::wstring(scripts_path.begin(), scripts_path.end()).c_str());
         
	 String args = Sim()->getCfg()->getString(String("hooks/script") + itostr(i) + "args");
         char *argv[] = { (char*)(scriptname.c_str()), (char*)(args.c_str()) };

	 status = PyConfig_SetBytesArgv(&config, 2, argv);
	 if (PyStatus_Exception(status)){
	         fprintf(stderr, "Sniper: cannot open Python script %s\n, argv error", scriptname.c_str());
	         exit(-1);
	 }

	 status = Py_InitializeFromConfig(&config);
	 if (PyStatus_Exception(status)){
	         fprintf(stderr, "Sniper: cannot open Python script %s\n, config error", scriptname.c_str());
	         exit(-1);
	 }
	 PyConfig_Clear(&config);

         printf("Executing Python script %s\n", scriptname.c_str());
         int s = PyRun_SimpleFileEx(fopen(scriptname.c_str(), "r"), scriptname.c_str(), 1 /* closeit */);
         if (s != 0) {
            PyErr_Print();
            fprintf(stderr, "Sniper Error running Python script %s\n", scriptname.c_str());
            exit(-1);
         }
      }
   }
}

std::string HooksPy::get_root(){
	std::string sim_root;
	const char env_roots[2][16] = {"SNIPER_ROOT", "GRAPHITE_ROOT"};
	for (unsigned int i = 0 ; i < 2 ; i++)
	{
	   sim_root = getenv(env_roots[i]);
	   if (!sim_root.empty())
	      break;
	}
	LOG_ASSERT_ERROR(!sim_root.empty(), "Please make sure SNIPER_ROOT or GRAPHITE_ROOT is set");

	return sim_root;
}

void HooksPy::set_env(){
	if(pyInit) return;
	//This allows python script to call: `import sim_config` directly
	PyImport_AppendInittab("sim_config", PyInit_sim_config);
	PyImport_AppendInittab("sim_stats", PyInit_sim_stats);
	PyImport_AppendInittab("sim_hooks", PyInit_sim_hooks);
	PyImport_AppendInittab("sim_dvfs", PyInit_sim_dvfs);
	PyImport_AppendInittab("sim_control", PyInit_sim_control);
	PyImport_AppendInittab("sim_bbv", PyInit_sim_bbv);
	PyImport_AppendInittab("sim_mem", PyInit_sim_mem);
	PyImport_AppendInittab("sim_thread", PyInit_sim_thread);
	pyInit = true;
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
