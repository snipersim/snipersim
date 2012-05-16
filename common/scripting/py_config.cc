#include "hooks_py.h"
#include "simulator.h"
#include "config.hpp"

static PyObject *
getConfigString(PyObject *self, PyObject *args)
{
   const char *key = NULL;
   SInt64 index = -1;
   String result = "";

   if (!PyArg_ParseTuple(args, "s|i", &key, &index))
      return NULL;

   if (index != -1)
      result = Sim()->getCfg()->getStringArray(key, index);
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
