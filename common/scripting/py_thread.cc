#include "hooks_py.h"
#include "simulator.h"
#include "thread_manager.h"
#include "thread.h"
#include "scheduler.h"

static PyObject *
getNthreads(PyObject *self, PyObject *args)
{
   return PyInt_FromLong(Sim()->getThreadManager()->getNumThreads());
}

static PyObject *
getThreadAppid(PyObject *self, PyObject *args)
{
   unsigned long thread_id = INVALID_THREAD_ID;

   if (!PyArg_ParseTuple(args, "l", &thread_id))
      return NULL;

   if (thread_id >= Sim()->getThreadManager()->getNumThreads())
   {
      PyErr_SetString(PyExc_ValueError, "Invalid thread id");
      return NULL;
   }

   Thread *thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
   if (!thread)
   {
      PyErr_SetString(PyExc_ValueError, "Invalid thread id");
      return NULL;
   }

   return PyInt_FromLong(thread->getAppId());
}

static PyObject *
getThreadName(PyObject *self, PyObject *args)
{
   unsigned long thread_id = INVALID_THREAD_ID;

   if (!PyArg_ParseTuple(args, "l", &thread_id))
      return NULL;

   if (thread_id >= Sim()->getThreadManager()->getNumThreads())
   {
      PyErr_SetString(PyExc_ValueError, "Invalid thread id");
      return NULL;
   }

   Thread *thread = Sim()->getThreadManager()->getThreadFromID(thread_id);
   if (!thread)
   {
      PyErr_SetString(PyExc_ValueError, "Invalid thread id");
      return NULL;
   }

   return Py_BuildValue("s", thread->getName().c_str());
}

static PyObject *
getThreadAffinity(PyObject *self, PyObject *args)
{
   unsigned long thread_id = INVALID_THREAD_ID;

   if (!PyArg_ParseTuple(args, "l", &thread_id))
      return NULL;

   if (thread_id >= Sim()->getThreadManager()->getNumThreads())
   {
      PyErr_SetString(PyExc_ValueError, "Invalid thread id");
      return NULL;
   }

   cpu_set_t mask;
   bool success = Sim()->getThreadManager()->getScheduler()->threadGetAffinity(thread_id, sizeof(cpu_set_t), &mask);
   if (!success)
   {
      PyErr_SetString(PyExc_ValueError, "threadGetAffinity() failed");
      return NULL;
   }

   PyObject *res = PyTuple_New(Sim()->getConfig()->getApplicationCores());
   for(unsigned int cpu = 0; cpu < Sim()->getConfig()->getApplicationCores(); ++cpu)
   {
      PyObject *_res = CPU_ISSET(cpu, &mask) ? Py_True : Py_False;
      Py_INCREF(_res);
      PyTuple_SET_ITEM(res, cpu, _res);
   }

   return res;
}

static PyObject *
setThreadAffinity(PyObject *self, PyObject *args)
{
   unsigned long thread_id = INVALID_THREAD_ID;
   PyObject *py_mask;

   if (!PyArg_ParseTuple(args, "lO", &thread_id, &py_mask))
      return NULL;

   if (thread_id >= Sim()->getThreadManager()->getNumThreads())
   {
      PyErr_SetString(PyExc_ValueError, "Invalid thread id");
      return NULL;
   }

   if (!PySequence_Check(py_mask))
   {
      PyErr_SetString(PyExc_ValueError, "Second argument must be iteratable");
      return NULL;
   }

   if (PySequence_Size(py_mask) > (Py_ssize_t)Sim()->getConfig()->getApplicationCores())
   {
      PyErr_SetString(PyExc_ValueError, "Core mask is longer than number of available cores");
      return NULL;
   }

   cpu_set_t mask;
   CPU_ZERO(&mask);
   for(unsigned int cpu = 0; cpu < (unsigned int)PySequence_Size(py_mask) && cpu < 8*sizeof(cpu_set_t); ++cpu)
   {
      PyObject *item = PySequence_ITEM(py_mask, cpu);
      if (PyObject_IsTrue(item))
         CPU_SET(cpu, &mask);
      Py_DECREF(item);
   }

   bool result = Sim()->getThreadManager()->getScheduler()->threadSetAffinity(INVALID_THREAD_ID, thread_id, sizeof(cpu_set_t), &mask);

   if (result)
      Py_RETURN_TRUE;
   else
      Py_RETURN_FALSE;
}

static PyMethodDef PyThreadMethods[] = {
   { "get_nthreads", getNthreads, METH_VARARGS, "Get number of threads" },
   { "get_thread_appid", getThreadAppid, METH_VARARGS, "Get application ID for a thread" },
   { "get_thread_name", getThreadName, METH_VARARGS, "Get thread name" },
   { "get_thread_affinity", getThreadAffinity, METH_VARARGS, "Get thread affinity" },
   { "set_thread_affinity", setThreadAffinity, METH_VARARGS, "Set thread affinity" },
   { NULL, NULL, 0, NULL } /* Sentinel */
};

void HooksPy::PyThread::setup(void)
{
   Py_InitModule("sim_thread", PyThreadMethods);
}
