#include "hooks_py.h"
#include "simulator.h"
#include "config.hpp"

static PyObject *
getConfigString(PyObject *self, PyObject *args)
{
   const char *key = NULL;
   SInt32 index = -1;
   String result = "";

   if (!PyArg_ParseTuple(args, "s|i", &key, &index))
      return NULL;

   if (index != -1)
      result = Sim()->getCfg()->getStringArray(key, index);
   else
      result = Sim()->getCfg()->getString(key);

   return PyUnicode_FromString(result.c_str());
}

static PyObject *
getConfigInt(PyObject *self, PyObject *args)
{
   const char *key = NULL;
   SInt32 index = -1;
   SInt64 result = 0;

   if (!PyArg_ParseTuple(args, "s|i", &key, &index))
      return NULL;

   if (index != -1)
      result = Sim()->getCfg()->getIntArray(key, index);
   else
      result = Sim()->getCfg()->getInt(key);

   return PyLong_FromLongLong(result);
}

static PyObject *
getConfigFloat(PyObject *self, PyObject *args)
{
   const char *key = NULL;
   SInt32 index = -1;
   double result = 0;

   if (!PyArg_ParseTuple(args, "s|i", &key, &index))
      return NULL;

   if (index != -1)
      result = Sim()->getCfg()->getFloatArray(key, index);
   else
      result = Sim()->getCfg()->getFloat(key);

   return PyFloat_FromDouble(result);
}

static PyObject *
getConfigBool(PyObject *self, PyObject *args)
{
   const char *key = NULL;
   SInt32 index = -1;
   bool result = false;

   if (!PyArg_ParseTuple(args, "s|i", &key, &index))
      return NULL;

   if (index != -1)
      result = Sim()->getCfg()->getBoolArray(key, index);
   else
      result = Sim()->getCfg()->getBool(key);

   return PyBool_FromLong(result);
}


static PyMethodDef PySniperConfigMethods[] = {
   {"get",  getConfigString, METH_VARARGS, "Get configuration variable."},
   {"get_int",  getConfigInt, METH_VARARGS, "Get configuration variable."},
   {"get_float",  getConfigFloat, METH_VARARGS, "Get configuration variable."},
   {"get_bool",  getConfigBool, METH_VARARGS, "Get configuration variable."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

static PyModuleDef PySniperConfigModule = {
	PyModuleDef_HEAD_INIT,
	"sim_config",
	"",
	-1,
	PySniperConfigMethods,
	NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_sim_config(void)
{
   PyObject *pModule = PyModule_Create(&PySniperConfigModule);

   PyObject *pOutputdir = PyUnicode_FromString(Sim()->getConfig()->formatOutputFileName("").c_str());
   PyObject_SetAttrString(pModule, "output_dir", pOutputdir);
   Py_DECREF(pOutputdir);

   PyObject *pNcores = PyLong_FromLong(Sim()->getConfig()->getApplicationCores());
   PyObject_SetAttrString(pModule, "ncores", pNcores);
   Py_DECREF(pNcores);

   return pModule;
}
