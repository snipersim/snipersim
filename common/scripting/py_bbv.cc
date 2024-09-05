#include "hooks_py.h"
#include "simulator.h"
#include "core_manager.h"
#include "core.h"

//////////
// get(): retrieve a core's BBV
//////////

static PyObject *
enableBbv(PyObject *self, PyObject *args)
{
   Sim()->getConfig()->setBBVsEnabled(true);
   Py_RETURN_NONE;
}

static PyObject *
disableBbv(PyObject *self, PyObject *args)
{
   Sim()->getConfig()->setBBVsEnabled(false);
   Py_RETURN_NONE;
}

static PyObject *
getBbv(PyObject *self, PyObject *args)
{
   core_id_t core_id = -1;

   if (!PyArg_ParseTuple(args, "l", &core_id))
      return NULL;

   if (core_id >= (core_id_t)Sim()->getConfig()->getApplicationCores()) {
      PyErr_SetString(PyExc_ValueError, "Core does not exist");
      return NULL;
   }

   BbvCount *bbv = Sim()->getCoreManager()->getCoreFromID(core_id)->getBbvCount();

   PyObject *pRet = PyTuple_New(2);
   PyObject *pInstrs = PyLong_FromUnsignedLong(bbv->getInstructionCount());
   PyTuple_SET_ITEM(pRet, 0, pInstrs);

   PyObject *pBbv = PyTuple_New(BbvCount::NUM_BBV);
   for(int i = 0; i < BbvCount::NUM_BBV; ++i)
      PyTuple_SET_ITEM(pBbv, i, PyLong_FromUnsignedLong(bbv->getDimension(i)));
   PyTuple_SET_ITEM(pRet, 1, pBbv);

   return pRet;
}


//////////
// module definition
//////////

static PyMethodDef PyBbvMethods[] = {
   {"enable", enableBbv, METH_VARARGS, "Enable BBV collection."},
   {"disable", disableBbv, METH_VARARGS, "Enable BBV collection."},
   {"get",  getBbv, METH_VARARGS, "Retrieve cummulative BBV for core."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

static PyModuleDef PyBbvModule = {
	PyModuleDef_HEAD_INIT,
	"sim_bbv",
	"",
	-1,
	PyBbvMethods,
	NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_sim_bbv(void)
{
   PyObject *pModule = PyModule_Create(&PyBbvModule);

   PyObject *pGlobalConst = PyLong_FromLong(BbvCount::NUM_BBV);
   PyObject_SetAttrString(pModule, "BBV_SIZE", pGlobalConst);
   Py_DECREF(pGlobalConst);
   return pModule;
}
