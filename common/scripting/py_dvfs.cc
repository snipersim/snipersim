#include "hooks_py.h"
#include "simulator.h"
#include "dvfs_manager.h"
#include "magic_server.h"


static const ComponentPeriod * getDomain(SInt64 domain_id, bool allow_global)
{
   if (domain_id < 0) {
      if (allow_global) {
         switch(domain_id) {
            case -1:
               return Sim()->getDvfsManager()->getGlobalDomain();
            default:
               PyErr_SetString(PyExc_ValueError, "Invalid global domain ID");
               return NULL;
         }
      } else {
         PyErr_SetString(PyExc_ValueError, "Can only set core frequency, not global domains");
         return NULL;
      }
   } else {
      if (domain_id >= Sim()->getConfig()->getApplicationCores()) {
         PyErr_SetString(PyExc_ValueError, "Invalid core ID");
         return NULL;
      }else
         return Sim()->getDvfsManager()->getCoreDomain(domain_id);
   }
   return NULL;
}

static PyObject *
getFrequency(PyObject *self, PyObject *args)
{
   long int domain_id = -999;

   if (!PyArg_ParseTuple(args, "l", &domain_id))
      return NULL;

   const ComponentPeriod *domain = getDomain(domain_id, true);
   if (!domain)
      return NULL;

   UInt64 freq = 1000000000 / domain->getPeriod().getFS();

   return PyInt_FromLong(freq);
}

static PyObject *
setFrequency(PyObject *self, PyObject *args)
{
   long int core_id = -999;
   long int freq_mhz = -1;

   if (!PyArg_ParseTuple(args, "ll", &core_id, &freq_mhz))
      return NULL;

   const ComponentPeriod *domain = getDomain(core_id, false);
   if (!domain)
      return NULL;

   // We're running in a hook so we already have the thread lock, call MagicServer directly
   Sim()->getMagicServer()->setFrequency(core_id, freq_mhz);

   Py_RETURN_NONE;
}


static PyMethodDef PyDvfsMethods[] = {
   {"get_frequency",  getFrequency, METH_VARARGS, "Get core or global frequency, in MHz."},
   {"set_frequency",  setFrequency, METH_VARARGS, "Set core frequency, in MHz."},
   {NULL, NULL, 0, NULL} /* Sentinel */
};

void HooksPy::PyDvfs::setup(void)
{
   PyObject *pModule = Py_InitModule("sim_dvfs", PyDvfsMethods);

   PyObject *pGlobalConst = PyInt_FromLong(-1);
   PyObject_SetAttrString(pModule, "GLOBAL", pGlobalConst);
   Py_DECREF(pGlobalConst);
}
