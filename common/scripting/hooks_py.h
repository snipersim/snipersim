#ifndef HOOKS_PY_H
#define HOOKS_PY_H

/* undef some macros to avoid redefined warnings */
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE
#include <Python.h>
#include <string>

#if PY_MAJOR_VERSION < 3 || PY_MINOR_VERSION < 8
#error "Python version does not support some features used. Please upgrade to Pyhton 3.8 or higher."
#endif

class HooksPy {
   public:
      static void init(void);
      static void set_env();
      static void fini(void);

      static PyObject * callPythonFunction(PyObject *pFunc, PyObject *pArgs);

      static void prepare_abort();
      static bool need_to_abort();
   private:
      static std::string get_root();
      static void run_python_file_with_argv(const std::string &filename, const std::string &argv_str);
      static bool pyInit;
      static bool abort;

      static PyThreadState *_save;
};

PyMODINIT_FUNC PyInit_sim_config(void);
PyMODINIT_FUNC PyInit_sim_stats(void);
PyMODINIT_FUNC PyInit_sim_hooks(void);
PyMODINIT_FUNC PyInit_sim_dvfs(void);
PyMODINIT_FUNC PyInit_sim_control(void);
PyMODINIT_FUNC PyInit_sim_bbv(void);
PyMODINIT_FUNC PyInit_sim_mem(void);
PyMODINIT_FUNC PyInit_sim_thread(void);

#endif // HOOKS_PY_H
