#include "hooks_py.h"
#include "simulator.h"
#include "core_manager.h"

static PyObject *
readMemory(PyObject *self, PyObject *args)
{
   long int core_id;
   UInt64 address = 0, size = 0;

   if (!PyArg_ParseTuple(args, "lll", &core_id, &address, &size))
      return NULL;

   Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
   LOG_ASSERT_ERROR(core != NULL, "Invalid core_id %d", core_id);

   char *buf = new char[size];
   core->accessMemory(Core::NONE, Core::READ, address, buf, size, Core::MEM_MODELED_NONE);

   PyObject *res = Py_BuildValue("s#", buf, size);
   delete buf;

   return res;
}

static PyObject *
readCstr(PyObject *self, PyObject *args)
{
   long int core_id;
   UInt64 address = 0;

   if (!PyArg_ParseTuple(args, "il", &core_id, &address))
      return NULL;

   Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
   LOG_ASSERT_ERROR(core != NULL, "Invalid core_id %d", core_id);

   const long maxsize = 65536, chunksize = 256;
   char *buf = new char[maxsize+1];
   char *ptr = buf;
   do
   {
      core->accessMemory(Core::NONE, Core::READ, address, ptr, chunksize, Core::MEM_MODELED_NONE);
      address += chunksize;
      ptr += chunksize;
   }
   while(memchr(buf, 0, ptr - buf) == NULL && ptr < buf + maxsize);

   buf[maxsize] = 0;
   PyObject *res = Py_BuildValue("s", buf);
   delete buf;

   return res;
}

static PyMethodDef PyMemMethods[] = {
   { "read", readMemory, METH_VARARGS, "Read memory (core, address, size)" },
   { "read_cstr", readCstr, METH_VARARGS, "Read null-terminated string (core, address)" },
   { NULL, NULL, 0, NULL } /* Sentinel */
};

void HooksPy::PyMem::setup(void)
{
   Py_InitModule("sim_mem", PyMemMethods);
}
