#include "hooks_py.h"
#include "simulator.h"
#include "clock_skew_minimization_object.h"
#include "stats.h"
#include "magic_server.h"


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

static PyTypeObject statsGetterType = {
   PyObject_HEAD_INIT(NULL)
   0,                         /*ob_size*/
   "statsGetter",             /*tp_name*/
   sizeof(statsGetterObject), /*tp_basicsize*/
   0,                         /*tp_itemsize*/
   0,                         /*tp_dealloc*/
   0,                         /*tp_print*/
   0,                         /*tp_getattr*/
   0,                         /*tp_setattr*/
   0,                         /*tp_compare*/
   0,                         /*tp_repr*/
   0,                         /*tp_as_number*/
   0,                         /*tp_as_sequence*/
   0,                         /*tp_as_mapping*/
   0,                         /*tp_hash */
   statsGetterGet,            /*tp_call*/
   0,                         /*tp_str*/
   0,                         /*tp_getattro*/
   0,                         /*tp_setattro*/
   0,                         /*tp_as_buffer*/
   Py_TPFLAGS_DEFAULT,        /*tp_flags*/
   "Stats getter objects",    /*tp_doc*/
   0,                         /*tp_traverse*/
   0,                         /*tp_clear*/
   0,                         /*tp_richcompare*/
   0,                         /*tp_weaklistoffset*/
   0,                         /*tp_iter*/
   0,                         /*tp_iternext*/
   0,                         /*tp_methods*/
   0,                         /*tp_members*/
   0,                         /*tp_getset*/
   0,                         /*tp_base*/
   0,                         /*tp_dict*/
   0,                         /*tp_descr_get*/
   0,                         /*tp_descr_set*/
   0,                         /*tp_dictoffset*/
   0,                         /*tp_init*/
   0,                         /*tp_alloc*/
   0,                         /*tp_new*/
   0,                         /*tp_free*/
   0,                         /*tp_is_gc*/
   0,                         /*tp_bases*/
   0,                         /*tp_mro*/
   0,                         /*tp_cache*/
   0,                         /*tp_subclasses*/
   0,                         /*tp_weaklist*/
   0,                         /*tp_del*/
   0,                         /*tp_version_tag*/
};

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

static String statsCallback(String objectName, UInt32 index, String metricName, PyObject *pFunc)
{
   PyObject *pResult = HooksPy::callPythonFunction(pFunc, Py_BuildValue("(sls)", objectName.c_str(), index, metricName.c_str()));

   if (!PyString_Check(pResult)) {
      PyObject *pAsString = PyObject_Str(pResult);
      if (!pAsString) {
         fprintf(stderr, "Stats callback: return value must be (convertable into) string\n");
         Py_XDECREF(pResult);
         return "";
      }
      Py_XDECREF(pResult);
      pResult = pAsString;
   }

   String val(PyString_AsString(pResult));
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

   Sim()->getStatsManager()->registerMetric(new StatsMetricCallback(objectName, index, metricName, (StatsCallback)statsCallback, (void*)pFunc));

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
   {"marker", writeMarker, METH_VARARGS, "Record a marker (coreid, threadid, arg0, arg1, [description])."},
   {"time", getTime, METH_VARARGS, "Retrieve the current global time in femtoseconds (approximate, last barrier)."},
   {"icount", getIcount, METH_VARARGS, "Retrieve current global instruction count."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

void HooksPy::PyStats::setup(void)
{
   PyObject *pModule = Py_InitModule("sim_stats", PyStatsMethods);

   statsGetterType.tp_new = PyType_GenericNew;
   if (PyType_Ready(&statsGetterType) < 0)
      return;

   Py_INCREF(&statsGetterType);
   PyModule_AddObject(pModule, "Getter", (PyObject *)&statsGetterType);
}
