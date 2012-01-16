#ifndef __VM_MANAGER_H__
#define __VM_MANAGER_H__

#include <unistd.h>
#include <sys/mman.h>

#include "fixed_types.h"

class VMManager
{
   public:

      static VMManager *getSingleton();
      static void allocate();
      static void release();

      void *brk(void *end_data_segment);
      void *mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset);
      void *mmap2(void *start, size_t length, int prot, int flags, int fd, off_t offset);
      int munmap(void *start, size_t length);

   private:

      VMManager();
      ~VMManager();

      IntPtr m_start_data_segment;
      IntPtr m_end_data_segment;
      
      IntPtr m_start_stack_segment;
      IntPtr m_end_stack_segment;

      IntPtr m_start_dynamic_segment;
      IntPtr m_end_dynamic_segment;

      static VMManager *m_singleton;
};

struct mmap_arg_struct
{
   unsigned long addr;
   unsigned long len;
   unsigned long prot;
   unsigned long flags;
   unsigned long fd;
   unsigned long offset;
};


#endif /* __VM_MANAGER_H__ */
