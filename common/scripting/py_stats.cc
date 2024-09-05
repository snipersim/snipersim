#include "hooks_py.h"
#include "simulator.h"
#include "clock_skew_minimization_object.h"
#include "stats.h"
#include "magic_server.h"
#include "thread_stats_manager.h"


//////////
// get(): retrieve a stats value
//////////

static PyObject *
getStatsValue(PyObject *self, PyObject *args)
{
   const char *objectName = NULL, *metricName = NULL;
   long int index = -1;

   if (!PyArg_ParseTuple(args, "sls", &objectName, &index, &metricName))
      return NULL;

   StatsMetricBase *metric = Sim()->getStatsManager()->getMetricObject(objectName, index, metricName);

   if (!metric) {
      PyErr_SetString(PyExc_ValueError, "Stats metric not found");
      return NULL;
   }

   return PyLong_FromUnsignedLongLong(metric->recordMetric());
}


//////////
// getter(): return a statsGetterObject Python object which, when called, returns a stats value
//////////

typedef struct {
   PyObject_HEAD
   StatsMetricBase *metric;
} statsGetterObject;

static PyObject *
statsGetterGet(PyObject *self, PyObject *args, PyObject *kw)
{
   statsGetterObject *getter = (statsGetterObject *)self;
   StatsMetricBase *metric = getter->metric;
   return PyLong_FromUnsignedLongLong(metric->recordMetric());
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static PyTypeObject statsGetterType = {
   .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
   .tp_name = "statsGetter",
   .tp_basicsize = sizeof(statsGetterObject),
   .tp_call = statsGetterGet,
   .tp_flags = Py_TPFLAGS_DEFAULT,
   .tp_doc = PyDoc_STR("Stats getter objects"),
};
#pragma GCC diagnostic pop

static PyObject *
getStatsGetter(PyObject *self, PyObject *args)
{
   const char *objectName = NULL, *metricName = NULL;
   long int index = -1;

   if (!PyArg_ParseTuple(args, "sls", &objectName, &index, &metricName))
      return NULL;

   StatsMetricBase *metric = Sim()->getStatsManager()->getMetricObject(objectName, index, metricName);

   if (!metric) {
      PyErr_SetString(PyExc_ValueError, "Stats metric not found");
      return NULL;
   }

   statsGetterObject *pGetter = PyObject_New(statsGetterObject, &statsGetterType);
   pGetter->metric = metric;

   return (PyObject *)pGetter;
}


//////////
// write(): write the current set of statistics out to sim.stats or our own file
//////////

static PyObject *
writeStats(PyObject *self, PyObject *args)
{
   const char *prefix = NULL;

   if (!PyArg_ParseTuple(args, "s", &prefix))
      return NULL;

   Sim()->getStatsManager()->recordStats(prefix);

   Py_RETURN_NONE;
}


//////////
// register(): register a callback function that returns a statistics value
//////////

static UInt64 statsCallback(String objectName, UInt32 index, String metricName, UInt64 _pFunc)
{
   PyObject *pFunc = (PyObject*)_pFunc;
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(sls)", objectName.c_str(), index, metricName.c_str()));

   if (!pResult || !PyLong_Check(pResult)) {
      LOG_PRINT_WARNING("Stats callback: return value must be (convertable into) 64-bit unsigned integer");
      if (pResult)
         Py_XDECREF(pResult);
      return 0;
   }

   UInt64 val = PyLong_AsLongLong(pResult);
   Py_XDECREF(pResult);

   return val;
}

static PyObject *
registerStats(PyObject *self, PyObject *args)
{
   const char *objectName = NULL, *metricName = NULL;
   long int index = -1;
   PyObject *pFunc = NULL;

   if (!PyArg_ParseTuple(args, "slsO", &objectName, &index, &metricName, &pFunc))
      return NULL;

   if (!PyCallable_Check(pFunc)) {
      PyErr_SetString(PyExc_TypeError, "Fourth argument must be callable");
      return NULL;
   }
   Py_INCREF(pFunc);

   Sim()->getStatsManager()->registerMetric(new StatsMetricCallback(objectName, index, metricName, (StatsCallback)statsCallback, (UInt64)pFunc));

   Py_RETURN_NONE;
}

static PyObject *
registerPerThread(PyObject *self, PyObject *args)
{
   const char *objectName = NULL, *metricName = NULL, *sPerThread = NULL;

   if (!PyArg_ParseTuple(args, "sss", &sPerThread, &objectName, &metricName))
      return NULL;

   ThreadStatNamedStat::registerStat(strdup(sPerThread), objectName, metricName);

   Py_RETURN_NONE;
}


//////////
// marker(): record a marker
//////////

static PyObject *
writeMarker(PyObject *self, PyObject *args)
{
   UInt64 core_id = INVALID_CORE_ID, thread_id = INVALID_THREAD_ID, arg0 = 0, arg1 = 0;
   const char *description = NULL;

   if (!PyArg_ParseTuple(args, "llll|z", &core_id, &thread_id, &arg0, &arg1, &description))
      return NULL;

   Sim()->getStatsManager()->logMarker(Sim()->getClockSkewMinimizationServer()->getGlobalTime(), core_id, thread_id, arg0, arg1, description);

   Py_RETURN_NONE;
}


//////////
// time(): Return current global time in femtoseconds
//////////

static PyObject *
getTime(PyObject *self, PyObject *args)
{
   SubsecondTime time = Sim()->getClockSkewMinimizationServer()->getGlobalTime();
   return PyLong_FromUnsignedLongLong(time.getFS());
}


//////////
// icount(): Return current global instruction count
//////////

static PyObject *
getIcount(PyObject *self, PyObject *args)
{
   UInt64 icount = MagicServer::getGlobalInstructionCount();
   return PyLong_FromUnsignedLongLong(icount);
}


//////////
// module definition
//////////

static PyMethodDef PyStatsMethods[] = {
   {"get",  getStatsValue, METH_VARARGS, "Retrieve current value of statistic (objectName, index, metricName)."},
   {"getter", getStatsGetter, METH_VARARGS, "Return object to retrieve statistics value."},
   {"write", writeStats, METH_VARARGS, "Write statistics (<prefix>, [<filename>])."},
   {"register", registerStats, METH_VARARGS, "Register callback that defines statistics value for (objectName, index, metricName)."},
   {"register_per_thread", registerPerThread, METH_VARARGS, "Add a per-thread statistic (perthreadName) based on a named statistic (objectName, metricName)."},
   {"marker", writeMarker, METH_VARARGS, "Record a marker (coreid, threadid, arg0, arg1, [description])."},
   {"time", getTime, METH_VARARGS, "Retrieve the current global time in femtoseconds (approximate, last barrier)."},
   {"icount", getIcount, METH_VARARGS, "Retrieve current global instruction count."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

static PyModuleDef PyStatsModule = {
	PyModuleDef_HEAD_INIT,
	"sim_stats",
	"",
	-1,
	PyStatsMethods,
	NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_sim_stats(void)
{
   PyObject *pModule = PyModule_Create(&PyStatsModule);

   statsGetterType.tp_new = PyType_GenericNew;
   if (PyType_Ready(&statsGetterType) < 0)
      return NULL;

   Py_INCREF(&statsGetterType);
   PyModule_AddObject(pModule, "Getter", (PyObject *)&statsGetterType);
   return pModule;
}

