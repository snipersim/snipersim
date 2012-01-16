#include "syscall_model.h"
#include "sys/syscall.h"
#include "transport.h"
#include "config.h"

// --------------------------------------------
// New stuff added with Memory redirection

#include "simulator.h"
#include "core.h"
#include "core_manager.h"
#include "vm_manager.h"
#include "performance_model.h"
#include "pthread_emu.h"

#include <errno.h>

// --- included for syscall: fstat
#include <sys/stat.h>

// ---- included for syscall: ioctl
#include <sys/ioctl.h>
#include <termios.h>

// ----- Included for getrlimit / setrlimit
#include <sys/time.h>
#include <sys/resource.h>

// ------ Included for rt_sigaction, rt_sigprocmask, rt_sigsuspend, sigreturn, kill
#include <signal.h>

// ------ Included for readahead
#include <fcntl.h>

// ------ Included for writev
#include <sys/uio.h>

#include <linux/futex.h>

#include "futex_emu.h"

SyscallMdl::SyscallMdl(Network *net)
      : m_called_enter(false),
      m_ret_val(0),
      m_network(net)
{
}

// --------------------------------------------
// New stuff added with Memory redirection

void SyscallMdl::saveSyscallNumber(IntPtr syscall_number)
{
   m_syscall_number = syscall_number;
}

IntPtr SyscallMdl::retrieveSyscallNumber()
{
   return m_syscall_number;
}

void SyscallMdl::saveSyscallArgs(syscall_args_t &args)
{
   m_saved_args.arg0 = args.arg0;
   m_saved_args.arg1 = args.arg1;
   m_saved_args.arg2 = args.arg2;
   m_saved_args.arg3 = args.arg3;
   m_saved_args.arg4 = args.arg4;
   m_saved_args.arg5 = args.arg5;
}

void SyscallMdl::retrieveSyscallArgs(syscall_args_t &args)
{
   args.arg0 = m_saved_args.arg0;
   args.arg1 = m_saved_args.arg1;
   args.arg2 = m_saved_args.arg2;
   args.arg3 = m_saved_args.arg3;
   args.arg4 = m_saved_args.arg4;
   args.arg5 = m_saved_args.arg5;
}

void* SyscallMdl::copyArgToBuffer(UInt32 arg_num, IntPtr arg_addr, UInt32 size)
{
   assert (arg_num < m_num_syscall_args);
   assert (size < m_scratchpad_size);
   char *scratchpad = m_scratchpad [arg_num];
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->accessMemory (Core::NONE, Core::READ, arg_addr, scratchpad, size);
   return (void*) scratchpad;
}

void SyscallMdl::copyArgFromBuffer(UInt32 arg_num, IntPtr arg_addr, UInt32 size)
{
   assert (arg_num < m_num_syscall_args);
   assert (size < m_scratchpad_size);
   char *scratchpad = m_scratchpad[arg_num];
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->accessMemory(Core::NONE, Core::WRITE, arg_addr, scratchpad, size);
}

// --------------------------------------------

IntPtr SyscallMdl::runExit(IntPtr old_return)
{
   if (m_called_enter)
   {
      m_called_enter = false;
      return m_ret_val;
   }
   else
   {
      return old_return;
   }
}

IntPtr SyscallMdl::runEnter(IntPtr syscall_number, syscall_args_t &args)
{
   LOG_PRINT("Got Syscall: %i", syscall_number);

   // Reset the buffers for the new transmission
   m_recv_buff.clear();
   m_send_buff.clear();

   int msg_type = MCP_MESSAGE_SYS_CALL;

   m_send_buff << msg_type << syscall_number;

   switch (syscall_number)
   {
      case SYS_open:
            m_called_enter = true;
            m_ret_val = marshallOpenCall(args);
            break;

      case SYS_read:
            m_called_enter = true;
            m_ret_val = marshallReadCall(args);
            break;

      case SYS_write:
            m_called_enter = true;
            m_ret_val = marshallWriteCall(args);
            break;

      case SYS_writev:
            m_called_enter = true;
            m_ret_val = marshallWritevCall(args);
            break;

      case SYS_close:
            m_called_enter = true;
            m_ret_val = marshallCloseCall(args);
            break;

      case SYS_lseek:
            m_called_enter = true;
            m_ret_val = marshallLseekCall(args);
            break;

      case SYS_access:
            m_called_enter = true;
            m_ret_val = marshallAccessCall(args);
            break;

#ifdef TARGET_X86_64
      case SYS_stat:
      case SYS_lstat:
         // Same as stat() except that it stats a link
         m_called_enter = true;
         m_ret_val = marshallStatCall(args);
         break;

      case SYS_fstat:
         m_called_enter = true;
         m_ret_val = marshallFstatCall(args);
         break;
#endif

#ifdef TARGET_IA32
      case SYS_fstat64:
         m_called_enter = true;
         m_ret_val = marshallFstat64Call(args);
         break;
#endif

      case SYS_ioctl:
         m_called_enter = true;
         m_ret_val = marshallIoctlCall(args);
         break;

      case SYS_getpid:
         m_called_enter = true;
         m_ret_val = marshallGetpidCall (args);
         break;

      case SYS_readahead:
         m_called_enter = true;
         m_ret_val = marshallReadaheadCall (args);
         break;

      case SYS_pipe:
         m_called_enter = true;
         m_ret_val = marshallPipeCall (args);
         break;

      case SYS_mmap:
         m_called_enter = true;
         m_ret_val = marshallMmapCall (args);
         break;

#ifdef TARGET_IA32
      case SYS_mmap2:
         m_called_enter = true;
         m_ret_val = marshallMmap2Call (args);
         break;
#endif

      case SYS_munmap:
         m_called_enter = true;
         m_ret_val = marshallMunmapCall (args);
         break;

      case SYS_brk:
         m_called_enter = true;
         m_ret_val = marshallBrkCall (args);
         break;

      case SYS_futex:
         m_called_enter = true;
         m_ret_val = marshallFutexCall (args);
         break;

      case -1:
      default:
         break;
   }

   LOG_PRINT("Syscall finished");

   return m_called_enter ? SYS_getpid : syscall_number;
}

IntPtr SyscallMdl::marshallOpenCall(syscall_args_t &args)
{
   /*
       Syscall Args
       const char *pathname, int flags


       Transmit Protocol

       Field               Type
       -----------------|--------
       LEN_FNAME           UInt32
       FILE_NAME           char[]
       STATUS_FLAGS        int
       MODE                UInt64

       Receive Protocol

       Field               Type
       -----------------|--------
       STATUS              int

   */

   char *path = (char *)args.arg0;
   int flags = (int)args.arg1;
   UInt64 mode = (UInt64) args.arg2;

   UInt32 len_fname = getStrLen (path) + 1;

   char *path_buf = new char [len_fname];
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->accessMemory (Core::NONE, Core::READ, (IntPtr) path, (char*) path_buf, len_fname);

   m_send_buff << len_fname << std::make_pair(path_buf, len_fname) << flags << mode;
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(int));
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   int status;
   m_recv_buff >> status;

   delete [] path_buf;
   delete [] (Byte*) recv_pkt.data;

   return status;
}


IntPtr SyscallMdl::marshallReadCall(syscall_args_t &args)
{

   /*
       Syscall Args
       int fd, void *buf, size_t count


       Transmit

       Field               Type
       -----------------|--------
       FILE_DESCRIPTOR     int
       COUNT               size_t

       Receive

       Field               Type
       -----------------|--------
       BYTES               int
       BUFFER              void *

   */

   int fd = (int)args.arg0;
   void *buf = (void *)args.arg1;
   size_t count = (size_t)args.arg2;

   // if shared mem, provide the buf to read into
   m_send_buff << fd << count;
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   assert(recv_pkt.length >= sizeof(int));
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   int bytes;
   m_recv_buff >> bytes;

   if (bytes != -1)
   {
      assert(m_recv_buff.size() == bytes);

      // Read data from MCP into a local buffer
      char* read_buf = new char[bytes];
      m_recv_buff >> std::make_pair(read_buf, bytes);

      // Write the data to memory
      Core* core = Sim()->getCoreManager()->getCurrentCore();
      core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) buf, read_buf, bytes);
   }
   else
   {
      assert(m_recv_buff.size() == 0);
   }

   delete [] (Byte*) recv_pkt.data;

   return bytes;
}

IntPtr SyscallMdl::marshallWriteCall(syscall_args_t &args)
{
   /*
       Syscall Args
       int fd, void *buf, size_t count


       Transmit

       Field               Type
       -----------------|--------
       FILE_DESCRIPTOR     int
       COUNT               size_t
       BUFFER              char[]

       Receive

       Field               Type
       -----------------|--------
       BYTES               int

   */

   int fd = (int)args.arg0;
   void *buf = (void *)args.arg1;
   size_t count = (size_t)args.arg2;

   char *write_buf = new char [count];
   // Always pass all the data in the message, even if shared memory is available
   // I think this is a reasonable model and is definitely one less thing to keep
   // track of when you switch between shared-memory/no shared-memory
   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->accessMemory (Core::NONE, Core::READ, (IntPtr) buf, (char*) write_buf, count);

   m_send_buff << fd << count << std::make_pair(write_buf, count);

   delete [] write_buf;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(int));
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   int status;
   m_recv_buff >> status;

   delete [] (Byte*) recv_pkt.data;

   return status;
}

IntPtr SyscallMdl::marshallWritevCall(syscall_args_t &args)
{
   //
   // Syscall Args
   // int fd, const struct iovec *iov, int iovcnt
   //
   // Transmit
   //
   // Field               Type
   // ------------------|---------
   // FILE DESCRIPTOR     int
   // COUNT               UInt64
   // BUFFER              char[]
   //
   // Receive
   //
   // Field               Type
   // ------------------|---------
   // BYTES               IntPtr

   int fd = (int) args.arg0;
   struct iovec *iov = (struct iovec*) args.arg1;
   int iovcnt = (int) args.arg2;

   Core *core = Sim()->getCoreManager()->getCurrentCore();

   struct iovec *iov_buf = new struct iovec [iovcnt];
   core->accessMemory(Core::NONE, Core::READ, (IntPtr) iov, (char*) iov_buf, iovcnt * sizeof (struct iovec));

   UInt64 count = 0;
   for (int i = 0; i < iovcnt; i++)
      count += iov_buf[i].iov_len;

   char *buf = new char[count];
   char* head = buf;
   int running_count = 0;

   for (int i = 0; i < iovcnt; i++)
   {
      core->accessMemory(Core::NONE, Core::READ, (IntPtr) iov_buf[i].iov_base, head, iov_buf[i].iov_len);
      running_count += iov_buf[i].iov_len;
      head = &buf[running_count];
   }

   m_send_buff << fd << count << std::make_pair(buf, count);

   delete [] buf;

   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(IntPtr));
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   IntPtr status;
   m_recv_buff >> status;

   delete [] (Byte*) recv_pkt.data;

   return status;
}

IntPtr SyscallMdl::marshallCloseCall(syscall_args_t &args)
{
   /*
       Syscall Args
       int fd


       Transmit

       Field               Type
       -----------------|--------
       FILE_DESCRIPTOR     int

       Receive

       Field               Type
       -----------------|--------
       STATUS              int

   */

   int fd = (int)args.arg0;

   m_send_buff << fd;
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   assert(recv_pkt.length == sizeof(int));
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   int status;
   m_recv_buff >> status;

   delete [] (Byte*) recv_pkt.data;

   return status;
}

IntPtr SyscallMdl::marshallLseekCall(syscall_args_t &args)
{
   int fd = (int) args.arg0;
   off_t offset = (off_t) args.arg1;
   int whence = (int) args.arg2;

   m_send_buff << fd << offset << whence ;
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);
   LOG_ASSERT_ERROR(recv_pkt.length == sizeof(off_t), "Recv Pkt length: expected(%u), got(%u)", sizeof(off_t), recv_pkt.length);
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   off_t ret_val;
   m_recv_buff >> ret_val;

   delete [] (Byte*) recv_pkt.data;

   return ret_val;
}

IntPtr SyscallMdl::marshallAccessCall(syscall_args_t &args)
{
   char *path = (char *)args.arg0;
   int mode = (int)args.arg1;

   UInt32 len_fname = getStrLen(path) + 1;
   char *path_buf = new char [len_fname];

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->accessMemory (Core::NONE, Core::READ, (IntPtr) path, (char*) path_buf, len_fname);

   // pack the data
   m_send_buff << len_fname << std::make_pair(path_buf, len_fname) << mode;

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get a result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   // return the result
   int result;
   m_recv_buff >> result;

   delete [] (Byte*) recv_pkt.data;
   delete [] path_buf;

   return result;
}

#ifdef TARGET_X86_64
IntPtr SyscallMdl::marshallStatCall(syscall_args_t &args)
{
   char *path = (char*) args.arg0;
   struct stat stat_buf;

   UInt32 len_fname = getStrLen(path) + 1;
   char* path_buf = new char[len_fname];

   Core* core = Sim()->getCoreManager()->getCurrentCore();
   // Read the data from memory
   core->accessMemory(Core::NONE, Core::READ, (IntPtr) path, (char*) path_buf, len_fname);
   core->accessMemory(Core::NONE, Core::READ, (IntPtr) args.arg1, (char*) &stat_buf, sizeof(struct stat));

   // pack the data
   m_send_buff << len_fname << std::make_pair(path_buf, len_fname);
   m_send_buff << std::make_pair(&stat_buf, sizeof(struct stat));

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get the result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   assert(m_recv_buff.size() == (sizeof(int) + sizeof(struct stat)));

   // Get the results
   int result;
   m_recv_buff.get<int>(result);
   m_recv_buff >> std::make_pair(&stat_buf, sizeof(struct stat));

   // Write the data to memory
   core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) args.arg1, (char*) &stat_buf, sizeof(struct stat));

   delete [] (Byte*) recv_pkt.data;
   delete [] path_buf;

   return result;
}

IntPtr SyscallMdl::marshallFstatCall(syscall_args_t &args)
{
   int fd = (int) args.arg0;
   struct stat buf;

   Core* core = Sim()->getCoreManager()->getCurrentCore();
   // Read the data from memory
   core->accessMemory(Core::NONE, Core::READ, (IntPtr) args.arg1, (char*) &buf, sizeof(struct stat));

   // pack the data
   m_send_buff.put<int>(fd);
   m_send_buff << std::make_pair(&buf, sizeof(struct stat));

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get the result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   assert(m_recv_buff.size() == (sizeof(int) + sizeof(struct stat)));

   // Get the results
   int result;
   m_recv_buff.get<int>(result);
   m_recv_buff >> std::make_pair(&buf, sizeof(struct stat));

   // Write the data to memory
   core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) args.arg1, (char*) &buf, sizeof(struct stat));

   delete [] (Byte*) recv_pkt.data;

   return result;
}
#endif

#ifdef TARGET_IA32
IntPtr SyscallMdl::marshallFstat64Call(syscall_args_t &args)
{
   int fd = (int) args.arg0;
   struct stat64 buf;

   Core* core = Sim()->getCoreManager()->getCurrentCore();
   // Read the data from memory
   core->accessMemory(Core::NONE, Core::READ, (IntPtr) args.arg1, (char*) &buf, sizeof(struct stat64));

   // pack the data
   m_send_buff.put<int>(fd);
   m_send_buff << std::make_pair(&buf, sizeof(struct stat64));

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get the result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   // Get the results
   int result;
   m_recv_buff.get<int>(result);
   m_recv_buff >> std::make_pair(&buf, sizeof(struct stat64));

   // Write the data to memory
   core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) args.arg1, (char*) &buf, sizeof(struct stat64));

   delete [] (Byte*) recv_pkt.data;

   return result;
}
#endif

IntPtr SyscallMdl::marshallIoctlCall(syscall_args_t &args)
{
   int fd = (int) args.arg0;
   int request = (int) args.arg1;

   LOG_ASSERT_ERROR(request == TCGETS, "ioctl() system call, only TCGETS request supported, request(0x%x)", request);

   struct termios buf;

   Core* core = Sim()->getCoreManager()->getCurrentCore();
   // Read the data from memory
   core->accessMemory(Core::NONE, Core::READ, (IntPtr) args.arg2, (char*) &buf, sizeof(struct termios));

   // pack the data
   m_send_buff.put<int>(fd);
   m_send_buff.put<int>(request);
   m_send_buff << std::make_pair(&buf, sizeof(struct termios));

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get the result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   // Get the results
   int result;
   m_recv_buff.get<int>(result);
   m_recv_buff >> std::make_pair(&buf, sizeof(struct termios));

   // Write the data to memory
   core->accessMemory(Core::NONE, Core::WRITE, (IntPtr) args.arg2, (char*) &buf, sizeof(struct termios));

   delete [] (Byte*) recv_pkt.data;

   return result;
}

IntPtr SyscallMdl::marshallGetpidCall (syscall_args_t &args)
{
   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get a result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   // return the result
   int result;
   m_recv_buff >> result;

   delete [] (Byte*) recv_pkt.data;

   return result;
}

IntPtr SyscallMdl::marshallReadaheadCall(syscall_args_t &args)
{
   int fd = (int) args.arg0;
   UInt32 offset_msb = (UInt32) args.arg1;
   UInt32 offset_lsb = (UInt32) args.arg2;
   size_t count = (size_t) args.arg3;

   off64_t offset = offset_msb;
   offset = offset << 32;
   offset += offset_lsb;

   // pack the data
   m_send_buff << fd << offset << count;

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get a result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   // return the result
   int result;
   m_recv_buff >> result;

   delete [] (Byte*) recv_pkt.data;

   return result;
}

IntPtr SyscallMdl::marshallPipeCall (syscall_args_t &args)
{
   int *fd = (int*) args.arg0;

   // send the data
   m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

   // get a result
   NetPacket recv_pkt;
   recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

   // Create a buffer out of the result
   m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

   // return the result
   int result;
   int fd_buff[2];

   m_recv_buff >> result;
   if (result == 0)
   {
      m_recv_buff >> fd_buff[0] >> fd_buff[1];
   }

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   core->accessMemory (Core::NONE, Core::WRITE, (IntPtr) fd, (char*) fd_buff, 2 * sizeof(int));

   delete [] (Byte*) recv_pkt.data;

   return result;
}

IntPtr SyscallMdl::marshallMmapCall (syscall_args_t &args)
{
   // --------------------------------------------
   // Syscall arguments:
   //
   // struct mmap_arg_struct *args
   //
   //  TRANSMIT
   //
   //  Field           Type
   //  --------------|------
   //  mmap_args_buf    mmap_arg_struct
   //
   //
   //  RECEIVE
   //
   //  Field           Type
   //  --------------|------
   //  start           void*
   //
   // --------------------------------------------

#ifdef TARGET_IA32
   struct mmap_arg_struct mmap_arg_buf;

   Core *core = Sim()->getCoreManager()->getCurrentCore();
   LOG_ASSERT_ERROR(core != NULL, "Core should not be null");
   core->accessMemory (Core::NONE, Core::READ, (IntPtr) args.arg0, (char*) &mmap_arg_buf, sizeof(mmap_arg_buf));

   if (Config::getSingleton()->isSimulatingSharedMemory())
   {
      // These are all 32-bit values
      m_send_buff.put(mmap_arg_buf.addr);
      m_send_buff.put(mmap_arg_buf.len);
      m_send_buff.put(mmap_arg_buf.prot);
      m_send_buff.put(mmap_arg_buf.flags);
      m_send_buff.put(mmap_arg_buf.fd);
      m_send_buff.put(mmap_arg_buf.offset);

      // send the data
      m_network->netSend(Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

      // get a result
      NetPacket recv_pkt;
      recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

      // Create a buffer out of the result
      m_recv_buff << std::make_pair(recv_pkt.data, recv_pkt.length);

      // Return the result
      void *start;
      m_recv_buff.get(start);

      delete [] (Byte*) recv_pkt.data;
      return (carbon_reg_t) start;
   }
   else
   {
      return (carbon_reg_t) syscall (SYS_mmap, args.arg0);
   }
#endif

#ifdef TARGET_X86_64
   // --------------------------------------------
   // Syscall arguments:
   //
   //  void *start, size_t length, int prot, int flags, int fd, off_t pgoffset
   //  TRANSMIT
   //
   //  Field           Type
   //  --------------|------
   //  start           void*
   //  length          size_t
   //  prot            int
   //  flags           int
   //  fd              int
   //  pgoffset        off_t
   //
   //
   //  RECEIVE
   //
   //  Field           Type
   //  --------------|------
   //  start           void*
   //
   // --------------------------------------------


   void *start = (void*) args.arg0;
   size_t length = (size_t) args.arg1;
   int prot = (int) args.arg2;
   int flags = (int) args.arg3;
   int fd = (int) args.arg4;
   off_t pgoffset = (off_t) args.arg5;

   LOG_PRINT("start(%p), length(0x%x), prot(0x%x), flags(0x%x), fd(%i), pgoffset(%u)",
         start, length, prot, flags, fd, pgoffset);

   if (Config::getSingleton()->isSimulatingSharedMemory())
   {
      m_send_buff.put(start);
      m_send_buff.put(length);
      m_send_buff.put(prot);
      m_send_buff.put(flags);
      m_send_buff.put(fd);
      m_send_buff.put(pgoffset);

      // send the data
      m_network->netSend (Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

      // get a result
      NetPacket recv_pkt;
      recv_pkt = m_network->netRecv(Config::getSingleton()->getMCPCoreNum (), MCP_RESPONSE_TYPE);

      // Create a buffer out of the result
      m_recv_buff << std::make_pair (recv_pkt.data, recv_pkt.length);

      // Return the result
      void *addr;
      m_recv_buff.get(addr);

      // Delete the data buffer
      delete [] (Byte*) recv_pkt.data;

      return (carbon_reg_t) addr;
   }
   else
   {
      return (carbon_reg_t) syscall(SYS_mmap, start, length, prot, flags, fd, pgoffset);
   }
#endif

}

#ifdef TARGET_IA32
IntPtr SyscallMdl::marshallMmap2Call (syscall_args_t &args)
{
   // --------------------------------------------
   // Syscall arguments:
   //
   //  void *start, size_t length, int prot, int flags, int fd, off_t pgoffset
   //  TRANSMIT
   //
   //  Field           Type
   //  --------------|------
   //  start           void*
   //  length          size_t
   //  prot            int
   //  flags           int
   //  fd              int
   //  pgoffset        off_t
   //
   //
   //  RECEIVE
   //
   //  Field           Type
   //  --------------|------
   //  start           void*
   //
   // --------------------------------------------


   void *start = (void*) args.arg0;
   size_t length = (size_t) args.arg1;
   int prot = (int) args.arg2;
   int flags = (int) args.arg3;
   int fd = (int) args.arg4;
   off_t pgoffset = (off_t) args.arg5;

   if (Config::getSingleton()->isSimulatingSharedMemory())
   {
      m_send_buff.put (start);
      m_send_buff.put (length);
      m_send_buff.put (prot);
      m_send_buff.put (flags);
      m_send_buff.put (fd);
      m_send_buff.put (pgoffset);

      // send the data
      m_network->netSend (Config::getSingleton()->getMCPCoreNum (), MCP_REQUEST_TYPE, m_send_buff.getBuffer (), m_send_buff.size ());

      // get a result
      NetPacket recv_pkt;
      recv_pkt = m_network->netRecv (Config::getSingleton()->getMCPCoreNum (), MCP_RESPONSE_TYPE);

      // Create a buffer out of the result
      m_recv_buff << std::make_pair (recv_pkt.data, recv_pkt.length);

      // Return the result
      void *addr;
      m_recv_buff.get(addr);

      // Delete the data buffer
      delete [] (Byte*) recv_pkt.data;

      return (carbon_reg_t) addr;
   }
   else
   {
      return (carbon_reg_t) syscall(SYS_mmap2, start, length, prot, flags, fd, pgoffset);
   }
}
#endif

IntPtr SyscallMdl::marshallMunmapCall (syscall_args_t &args)
{
   // --------------------------------------------
   // Syscall arguments:
   //
   // struct mmap_arg_struct *args
   //
   //  TRANSMIT
   //
   //  Field           Type
   //  --------------|------
   //  start           void*
   //  length          size_t
   //
   //
   //  RECEIVE
   //
   //  Field           Type
   //  --------------|------
   //  ret_val         int
   //
   // --------------------------------------------


   void *start = (void*) args.arg0;
   size_t length = (size_t) args.arg1;

   if (Config::getSingleton()->isSimulatingSharedMemory())
   {
      m_send_buff.put (start);
      m_send_buff.put (length);

      // send the data
      m_network->netSend (Config::getSingleton()->getMCPCoreNum (), MCP_REQUEST_TYPE, m_send_buff.getBuffer (), m_send_buff.size ());

      // get a result
      NetPacket recv_pkt;
      recv_pkt = m_network->netRecv (Config::getSingleton()->getMCPCoreNum (), MCP_RESPONSE_TYPE);

      // Create a buffer out of the result
      m_recv_buff << std::make_pair (recv_pkt.data, recv_pkt.length);

      // Return the result
      int ret_val;
      m_recv_buff.get(ret_val);

      // Delete the data buffer
      delete [] (Byte*) recv_pkt.data;

      return (carbon_reg_t) ret_val;
   }
   else
   {
      return (carbon_reg_t) syscall (SYS_munmap, start, length);
   }
}

IntPtr SyscallMdl::marshallBrkCall (syscall_args_t &args)
{
   // --------------------------------------------
   // Syscall arguments:
   //
   //  TRANSMIT
   //
   //  Field               Type
   //  ------------------|------
   //  end_data_segment    void*
   //
   //
   //  RECEIVE
   //
   //  Field                        Type
   //  ---------------------------|------
   //  new_end_data_segment         void*
   //
   // --------------------------------------------

   void *end_data_segment = (void*) args.arg0;

   if (Config::getSingleton()->isSimulatingSharedMemory())
   {
      m_send_buff.put (end_data_segment);

      // send the data
      m_network->netSend (Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

      // get a result
      NetPacket recv_pkt;
      recv_pkt = m_network->netRecv (Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

      // Create a buffer out of the result
      m_recv_buff << std::make_pair (recv_pkt.data, recv_pkt.length);

      // Return the result
      void *new_end_data_segment;
      m_recv_buff.get (new_end_data_segment);

      // Delete the data buffer
      delete [] (Byte*) recv_pkt.data;

      return (carbon_reg_t) new_end_data_segment;
   }
   else
   {
      return (carbon_reg_t) syscall (SYS_brk, end_data_segment);
   }
}

IntPtr SyscallMdl::marshallFutexCall (syscall_args_t &args)
{
   int *uaddr = (int*) args.arg0;
   int op = (int) args.arg1;
   int cmd = (op & FUTEX_CMD_MASK) & ~FUTEX_PRIVATE_FLAG;
   int val = (int) args.arg2;
   int val2 = (int) args.arg3;
   const struct timespec *timeout = (const struct timespec*) args.arg3;
   int *uaddr2 = (int*) args.arg4;
   int val3 = (int) args.arg5;

   if (Config::getSingleton()->isSimulatingSharedMemory())
   {
      struct timespec timeout_buf;
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      LOG_ASSERT_ERROR(core != NULL, "Core should not be null");

      SubsecondTime start_time;
      SubsecondTime end_time;
      start_time = core->getPerformanceModel()->getElapsedTime();

      if (timeout != NULL && cmd == FUTEX_WAIT)
      {
         core->accessMemory(Core::NONE, Core::READ, (IntPtr) timeout, (char*) &timeout_buf, sizeof(timeout_buf));
      }

      m_send_buff.put(uaddr);
      m_send_buff.put(op);
      m_send_buff.put(val);

      if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
         int timeout_prefix;
         if (timeout == NULL)
         {
            timeout_prefix = 0;
            m_send_buff.put(timeout_prefix);
         }
         else
         {
            timeout_prefix = 1;
            m_send_buff.put(timeout_prefix);
            m_send_buff << std::make_pair((const void*) &timeout_buf, sizeof(timeout_buf));
         }
      } else {
            m_send_buff.put(val2);
      }

      m_send_buff.put(uaddr2);
      m_send_buff.put(val3);

      m_send_buff.put(start_time);

      // send the data
      m_network->netSend (Config::getSingleton()->getMCPCoreNum(), MCP_REQUEST_TYPE, m_send_buff.getBuffer(), m_send_buff.size());

      // Set the CoreState to 'STALLED'
      m_network->getCore()->setState(Core::STALLED);

      updateState(core, PthreadEmu::STATE_WAITING);

      // get a result
      NetPacket recv_pkt;
      recv_pkt = m_network->netRecv (Config::getSingleton()->getMCPCoreNum(), MCP_RESPONSE_TYPE);

      // Set the CoreState to 'RUNNING'
      m_network->getCore()->setState(Core::WAKING_UP);

      // Create a buffer out of the result
      m_recv_buff << std::make_pair (recv_pkt.data, recv_pkt.length);

      // Return the result
      int ret_val;
      m_recv_buff.get(ret_val);
      m_recv_buff.get(end_time);

      updateState(core, PthreadEmu::STATE_RUNNING, end_time - start_time);

      // For FUTEX_WAKE, end_time = start_time
      // Look at common/system/syscall_server.cc for this
      if (end_time > start_time)
         core->getPerformanceModel()->queueDynamicInstruction(new SyncInstruction(end_time - start_time, SyncInstruction::FUTEX));

      // Delete the data buffer
      delete [] (Byte*) recv_pkt.data;

      return (carbon_reg_t) ret_val;
   }
   else
   {
      return (carbon_reg_t) syscall (SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
   }
}

// Helper functions
UInt32 SyscallMdl::getStrLen (char *str)
{
   UInt32 len = 0;
   char c;
   char *ptr = str;
   while (1)
   {
      Core *core = Sim()->getCoreManager()->getCurrentCore();
      core->accessMemory (Core::NONE, Core::READ, (IntPtr) ptr, &c, sizeof(char));
      if (c != '\0')
      {
         len++;
         ptr++;
      }
      else
         break;
   }
   return len;
}

