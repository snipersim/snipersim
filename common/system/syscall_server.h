//
// FIXME: this is a hack.
//
// Includes temporary hooks for the syscall server. Otherwise, this is
// the syscall server. The runSyscallServer hook function is to be
// manually inserted in main thread of the user app. It calls the code
// the implements the real server. Putting the hook in the user code's
// main thread gives the server a place to run; we avoid having to spawn
//
// a thread or something in the simulator to house the server code.
#ifndef SYSCALL_SERVER_H
#define SYSCALL_SERVER_H

#include "fixed_types.h"
#include "packetize.h"
#include "transport.h"
#include "network.h"

#include <iostream>
#include <unordered_map>
#include <queue>

// -- For futexes --
#include <linux/futex.h>
#include <sys/time.h>
#include <errno.h>

// -- Special Class to Handle Futexes
class SimFutex
{
   private:
      typedef std::queue<core_id_t> ThreadQueue;
      ThreadQueue m_waiting;

   public:
      SimFutex();
      ~SimFutex();
      void enqueueWaiter(core_id_t core_id, SubsecondTime time);
      core_id_t dequeueWaiter(core_id_t core_by, SubsecondTime time);
};

class SyscallServer
{
   public:
      SyscallServer(Network &network,
                    UnstructuredBuffer &send_buff_, UnstructuredBuffer &recv_buff_,
                    const UInt32 SERVER_MAX_BUFF,
                    char *scratch_);

      ~SyscallServer();

      void handleSyscall(core_id_t core_id);

   private:
      void marshallOpenCall(core_id_t core_id);
      void marshallReadCall(core_id_t core_id);
      void marshallWriteCall(core_id_t core_id);
      void marshallWritevCall(core_id_t core_id);
      void marshallCloseCall(core_id_t core_id);
      void marshallLseekCall(core_id_t core_id);
      void marshallGetcwdCall(core_id_t core_id);
      void marshallAccessCall(core_id_t core_id);
#ifdef TARGET_X86_64
      void marshallStatCall(IntPtr syscall_number, core_id_t core_id);
      void marshallFstatCall(core_id_t core_id);
#endif
#ifdef TARGET_IA32
      void marshallFstat64Call(core_id_t core_id);
#endif
      void marshallIoctlCall(core_id_t core_id);
      void marshallGetpidCall(core_id_t core_id);
      void marshallReadaheadCall(core_id_t core_id);
      void marshallPipeCall(core_id_t core_id);
      void marshallMmapCall(core_id_t core_id);
#ifdef TARGET_IA32
      void marshallMmap2Call(core_id_t core_id);
#endif
      void marshallMunmapCall(core_id_t core_id);
      void marshallBrkCall(core_id_t core_id);
      void marshallFutexCall(core_id_t core_id);

      // Handling Futexes
      void futexError(core_id_t core_id, SubsecondTime curr_time);
      void futexWait(core_id_t core_id, int *uaddr, int val, int act_val, SubsecondTime curr_time);
      void futexWake(core_id_t core_id, int *uaddr, int nr_wake, SubsecondTime curr_time);
      void futexWakeOp(core_id_t core_id, int op, int *uaddr, int val, int *uaddr2, int nr_wake, int nr_wake2, SubsecondTime curr_time);
      void futexCmpRequeue(core_id_t core_id, int *uaddr, int val, int *uaddr2, int val3, int act_val, SubsecondTime curr_time);
      core_id_t wakeFutexOne(SimFutex *sim_futex, core_id_t core_by, SubsecondTime curr_time);
      int futexDoOp(Core *core, int op, int *uaddr);

      //Note: These structures are shared with the MCP
   private:
      Network & m_network;
      UnstructuredBuffer & m_send_buff;
      UnstructuredBuffer & m_recv_buff;
      const UInt32 m_SYSCALL_SERVER_MAX_BUFF;
      char * const m_scratch;

      SubsecondTime m_reschedule_cost;

      // Handling Futexes
      typedef std::unordered_map<IntPtr, SimFutex> FutexMap;
      FutexMap m_futexes;


};


#endif
