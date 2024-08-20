#include "hooks_py.h"
#include "simulator.h"
#include "config.hpp"

bool HooksPy::pyInit = false;
bool HooksPy::abort = false;
PyThreadState* HooksPy::_save = NULL;

void HooksPy::init()
{
   //NOTE this modification to python3 is only tested with numscripts=1
   UInt64 numscripts = Sim()->getCfg()->getInt("hooks/numscripts");
   if (numscripts == 0) return;

   //Init the Python Interpreter
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
   
   status = Py_InitializeFromConfig(&config);
   if (PyStatus_Exception(status)){
           fprintf(stderr, "Sniper embedded Python, failed to intiailize the interpreter.");
           exit(-1);
   }
   PyConfig_Clear(&config);

   
   //Run the different scripts
   for(UInt64 i = 0; i < numscripts; ++i) {
      String scriptname_versa = Sim()->getCfg()->getString(String("hooks/script") + itostr(i) + "name");
      std::string scriptname(scriptname_versa.c_str(), scriptname_versa.size());
      if (scriptname.substr(scriptname.length()-3) == ".py") {
	      String args_versa = Sim()->getCfg()->getString(String("hooks/script") + itostr(i) + "args");
	      std::string args(args_versa.c_str(), args_versa.size());
	      run_python_file_with_argv(scriptname, args);
      }
   }
   
   //Release lock to the GIL
   HooksPy::_save = PyEval_SaveThread();
}

//this function assumes the thread has the GIL already
void HooksPy::run_python_file_with_argv(const std::string &filename, const std::string &argv_str){
    PyObject *sys_module, *sys_argv;

    sys_module = PyImport_ImportModule("sys");
    if (sys_module == NULL) {
        PyErr_Print();
        return;
    }

    // Get the sys.argv list
    sys_argv = PyObject_GetAttrString(sys_module, "argv");
    if (sys_argv == NULL || !PyList_Check(sys_argv)) {
        PyErr_Print();
        return;
    }

    // Clear the current sys.argv list
    PyList_SetSlice(sys_argv, 0, PyList_Size(sys_argv), NULL);

    // Add the filename as the first argument
    PyObject *script_name = PyUnicode_FromString(filename.c_str());
    if (script_name == NULL) {
            PyErr_Print();
            return;
    }
    PyList_Append(sys_argv, script_name);
    Py_DECREF(script_name);

    // Split the argv_str into individual arguments
    //argv_str could never contain any spaces because of the configuration parser. Argvs are pased through argv in run-sniper anyways.
    std::istringstream ss(argv_str);
    std::string arg;
    while (std::getline(ss, arg, ' ')) {
        PyObject *py_arg = PyUnicode_FromString(arg.c_str());
        if (py_arg == NULL) {
        	PyErr_Print();
                return;
        }
        PyList_Append(sys_argv, py_arg);
        Py_DECREF(py_arg);
    }

    // Open the Python file
    FILE *fp = fopen(filename.c_str(), "r");
    if (fp == NULL) {
        perror("fopen");
        return;
    }

    PyRun_SimpleFileEx(fp, filename.c_str(), 1);
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
   if (pyInit){
      PyEval_RestoreThread(HooksPy::_save);
//BUG? Python 3.12 hangs on this function, while we have the GIL
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION < 12
      Py_FinalizeEx();
#endif
   }
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

void HooksPy::prepare_abort(){
	HooksPy::abort = true;
}

bool HooksPy::need_to_abort(){
	return HooksPy::abort;
}
