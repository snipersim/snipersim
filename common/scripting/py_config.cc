#include "hooks_py.h"
#include "simulator.h"
#include "config.hpp"

static PyObject *
getConfigString(PyObject *self, PyObject *args)
{
   const char *key = NULL, *default_val = NULL;
   String result = "";

   if (!PyArg_ParseTuple(args, "s|s", &key, &default_val))
      return NULL;

   if (default_val)
      result = Sim()->getCfg()->getString(key, default_val);
   else
      result = Sim()->getCfg()->getString(key);

   return PyString_FromString(result.c_str());
}


static PyMethodDef PyConfigMethods[] = {
   {"get",  getConfigString, METH_VARARGS, "Get configuration variable."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

void HooksPy::PyConfig::setup(void)
{
   PyObject *pModule = Py_InitModule("sim_config", PyConfigMethods);

   PyObject *pOutputdir = PyString_FromString(Sim()->getConfig()->formatOutputFileName("").c_str());
   PyObject_SetAttrString(pModule, "output_dir", pOutputdir);
   Py_DECREF(pOutputdir);

   PyObject *pNcores = PyInt_FromLong(Sim()->getConfig()->getApplicationCores());
   PyObject_SetAttrString(pModule, "ncores", pNcores);
   Py_DECREF(pNcores);
}
