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
   set_env();

   // Sniper used to embed its own Python 2.7 kit, built --without-threads so the
   // whole simulator could run inside Pin.  The front end is a separate binary now
   // (SIFT channels), the kit is gone, and the build links a real python3 -- but
   // this code still pointed config.executable at $SIM_ROOT/python_kit/<arch>/bin/python,
   // a path nothing creates.  CPython fell back to the paths of the libpython it is
   // linked against, which is in fact the behaviour we want by default.
   //
   // Do not override config.executable unless asked.  Forcing it changes prefix and
   // exec_prefix computation, and for interpreters that build their extension modules
   // into the executable rather than into lib-dynload (python-build-standalone, for
   // one) that makes stdlib extensions such as _sqlite3 unimportable.  Set
   // SNIPER_PYTHON_EXECUTABLE to select an interpreter explicitly -- for a virtualenv,
   // point it at <venv>/bin/python so CPython finds the pyvenv.cfg beside it.
   const char *python_exe = getenv("SNIPER_PYTHON_EXECUTABLE");

   PyStatus status;
   PyConfig config;
   PyConfig_InitIsolatedConfig(&config);
	 
   if (python_exe)
   {
      // Setting executable lets CPython pick up a pyvenv.cfg in the directory above it.
      std::string python_exe_str(python_exe);
      status = PyConfig_SetString(&config, &config.executable,
                                  std::wstring(python_exe_str.begin(), python_exe_str.end()).c_str());
      if (PyStatus_Exception(status)){
         fprintf(stderr, "Cannot set executable path of Python config to '%s'\n", python_exe);
         exit(-1);
      }
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
      Py_FinalizeEx();
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
