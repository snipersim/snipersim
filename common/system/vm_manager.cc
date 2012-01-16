#include "vm_manager.h"
#include "simulator.h"
#include <boost/lexical_cast.hpp>
#include "config.hpp"

#include "log.h"

VMManager *VMManager::m_singleton = NULL;

void VMManager::allocate()
{
   assert (m_singleton == NULL);
   m_singleton = new VMManager();
}

void VMManager::release()
{
   delete m_singleton;
   m_singleton = NULL;
}

VMManager *VMManager::getSingleton()
{
   return m_singleton;
}

VMManager::VMManager()
{
   // FIXME: Figure out a way to get the end of the static data     
   m_start_data_segment = (IntPtr) sbrk(0);
   m_end_data_segment = m_start_data_segment;

   UInt32 total_cores = Sim()->getConfig()->getTotalCores();
  
   UInt32 stack_size_per_core;
   try
   {
      m_start_stack_segment = (IntPtr) boost::lexical_cast <unsigned long int> (Sim()->getCfg()->get("stack/stack_base"));
      stack_size_per_core = (UInt32) boost::lexical_cast <unsigned long int> (Sim()->getCfg()->get("stack/stack_size_per_core"));
   }
   catch (boost::bad_lexical_cast &)
   {
      LOG_PRINT_ERROR("Error reading stack parameters from the config file");
      return;
   }

   // FIXME: MCP does not have a stack. Do something about this
   m_end_stack_segment = m_start_stack_segment + total_cores * stack_size_per_core;

   LOG_ASSERT_ERROR(m_end_stack_segment > m_start_stack_segment,
       "Problem with Application Stack: start_stack_segment(0x%x), end_stack_segment(0x%x)",
       m_start_stack_segment, m_end_stack_segment); 

   LOG_ASSERT_ERROR(m_start_stack_segment > m_start_data_segment,
       "Problem with Application Stack: start_data_segment(0x%x), start_stack_segment(0x%x)",
       m_start_data_segment, m_start_stack_segment);

#ifdef TARGET_IA32
   m_start_dynamic_segment = 0xb0000000;
#endif

#ifdef TARGET_X86_64
   m_start_dynamic_segment = 0xf000000000;
#endif

   m_end_dynamic_segment = m_start_dynamic_segment;

   LOG_ASSERT_ERROR(m_start_dynamic_segment > m_end_stack_segment,
       "Problem with Application Stack: end_stack_segment(0x%x), start_dynamic_segment(0x%x)",
       m_end_stack_segment, m_start_dynamic_segment);
}

VMManager::~VMManager()
{
}

void *VMManager::brk(void *end_data_segment)
{
   LOG_PRINT("VMManager: brk(%p)", end_data_segment);

   if (end_data_segment == (void*) 0)
   {
      LOG_PRINT ("VMManager: brk(%p) returned %p", end_data_segment, m_end_data_segment);
      return (void*) m_end_data_segment;
   }

   LOG_ASSERT_ERROR(m_start_data_segment < ((IntPtr) end_data_segment),
       "Problem with brk() system call: start_data_segment(0x%x), end_data_segment(0x%x)",
       m_start_data_segment, (IntPtr) end_data_segment);

   LOG_ASSERT_ERROR(m_start_stack_segment > ((IntPtr) end_data_segment),
       "Problem with brk() system call: No more memory to allocate! end_data_segment(0x%x), start_stack_segment(0x%x)",
       (IntPtr) end_data_segment, m_start_stack_segment);

   m_end_data_segment = (IntPtr) end_data_segment;

   LOG_PRINT("VMManager: brk(%p) returned %p", end_data_segment, m_end_data_segment);
   return ((void*) m_end_data_segment);
}

void *VMManager::mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
   LOG_PRINT("VMManager: mmap(start = %p, length = 0x%x, flags = 0x%x, fd = %i, offset = %u)",
         start, length, flags, fd, flags);

   LOG_ASSERT_ERROR(fd == -1, 
         "Mmap() system call, received valid file descriptor. Not currently supported");
   LOG_ASSERT_ERROR((flags & MAP_ANONYMOUS) == MAP_ANONYMOUS,
         "Mmap() system call, MAP_ANONYMOUS should be set in flags");
   LOG_ASSERT_ERROR((flags & MAP_FIXED) == 0,
         "Mmap() system call, MAP_FIXED should NOT be set in flags");
   LOG_ASSERT_ERROR((flags & MAP_PRIVATE) == MAP_PRIVATE,
         "Mmap() system call, MAP_PRIVATE should be set in flags");
   
   LOG_ASSERT_ERROR(m_end_stack_segment < (m_start_dynamic_segment - length),
         "Mmap() system call: No more memory to allocate! end_stack_segment(0x%x), start_dynamic_segment(0x%x)",
         m_end_stack_segment, m_start_dynamic_segment - length);

   m_start_dynamic_segment -= length;

   LOG_PRINT("VMManager: mmap() returned %p", (void*) m_start_dynamic_segment);
   return ((void*) m_start_dynamic_segment);
}

#ifdef TARGET_IA32
void *VMManager::mmap2(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
   LOG_PRINT("VMManager: mmap2(start = %p, length = 0x%x, flags = 0x%x)",
         start, length, (unsigned) flags);

   LOG_ASSERT_ERROR(fd == -1, 
         "Mmap2() system call, received valid file descriptor. Not currently supported");
   LOG_ASSERT_ERROR((flags & MAP_ANONYMOUS) == MAP_ANONYMOUS,
         "Mmap2() system call, MAP_ANONYMOUS should be set in flags");
   LOG_ASSERT_ERROR((flags & MAP_FIXED) == 0,
         "Mmap2() system call, MAP_FIXED should NOT be set in flags");
   LOG_ASSERT_ERROR((flags & MAP_PRIVATE) == MAP_PRIVATE,
         "Mmap2() system call, MAP_PRIVATE should be set in flags");
   
   LOG_ASSERT_ERROR(m_end_stack_segment < (m_start_dynamic_segment - length),
         "Mmap2() system call: No more memory to allocate! end_stack_segment(0x%x), start_dynamic_segment(0x%x)",
         m_end_stack_segment, m_start_dynamic_segment - length);

   m_start_dynamic_segment -= length;

   LOG_PRINT("VMManager: mmap2() returned %p", (void*) m_start_dynamic_segment);
   return ((void*) m_start_dynamic_segment);
}
#endif

int VMManager::munmap(void *start, size_t length)
{
   LOG_PRINT("VMManager: munmap(start = %p, length = 0x%x",
         start, length);

   // Ignore for now
   LOG_ASSERT_ERROR(m_start_dynamic_segment <= ((IntPtr) start),
         "Munmap() system call, start(0x%x), start_dynamic_segment(0x%x)",
         (IntPtr) start, m_start_dynamic_segment);

   LOG_PRINT("VMManager: munmap() returned 0");
   return 0;
}
