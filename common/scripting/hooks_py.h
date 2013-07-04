#ifndef HOOKS_PY_H
#define HOOKS_PY_H

#include "fixed_types.h"

/* undef some macros to avoid redefined warnings */
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE
#include <Python.h>

class HooksPy {
   public:
      static void init(void);
      static void setup(void);
      static void fini(void);

      static PyObject * callPythonFunction(PyObject *pFunc, PyObject *pArgs);
   private:
      static bool pyInit;

      class PyConfig {
         public:
            static void setup(void);
      };
      class PyStats {
         public:
            static void setup(void);
      };
      class PyHooks {
         public:
            static void setup(void);
      };
      class PyDvfs {
         public:
            static void setup(void);
      };
      class PyControl {
          public:
              static void setup(void);
      };
      class PyBbv {
         public:
            static void setup(void);
      };
      class PyMem {
          public:
              static void setup(void);
      };
      class PyThread {
          public:
              static void setup(void);
      };
};

#endif // HOOKS_PY_H
