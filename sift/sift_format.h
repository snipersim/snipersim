#ifndef __SIFT_FORMAT_H
#define __SIFT_FORMAT_H

// Sniper Instruction Trace File Format
//
// ia32 and intel64, little-endian

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <sys/types.h>

#define NUM_PAPI_COUNTERS 6

// When compiling against PinCRT and not using Pinplay, disable zlib as we do not have a PinCRT-compiled version
#if defined(PIN_CRT) && !defined(PINPLAY)
# define SIFT_USE_ZLIB 0
#else
# define SIFT_USE_ZLIB 1
#endif

namespace Sift
{

   const uint32_t MagicNumber = 0x54464953; // "SIFT"

   /* The guest's time, the same in every front end.
      Its origin: TIME_START_DEFAULT seconds since the epoch, Sniper's
      osemu/time_start default (config/base.cfg), so that a guest reads the
      same clock under Sniper, the SIFT recorder and the QEMU plugin.
      Where the simulator does not answer -- a trace written to a file, a
      read no live channel carries -- the front end answers with a one-IPC
      model: one instruction per cycle on a core of ONE_IPC_HZ, an assumed
      1 GHz, counted from the thread's start.  Never the host's clock: its
      time has nothing to do with the program's.  The model's time is the
      thread's instruction count in nanoseconds, its cycles the instruction
      count, its instret the instruction count. */
   const uint64_t TIME_START_DEFAULT = 1337000000;  // seconds since the epoch
   const uint64_t ONE_IPC_HZ = 1000000000;          // the one-IPC model's core: 1 GHz, assumed
   const uint64_t NS_PER_S = 1000000000;
   /* Exact for any ONE_IPC_HZ (a 128-bit product: 1e9 / ONE_IPC_HZ is no
      integer above 1 GHz or for a rate that does not divide it), and no
      overflow of the product for any count */
   inline uint64_t one_ipc_ns(uint64_t instructions) { return (uint64_t)((unsigned __int128)instructions * NS_PER_S / ONE_IPC_HZ); }
   inline uint64_t one_ipc_cycles(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * ONE_IPC_HZ / NS_PER_S); }
   const uint64_t PAGE_SIZE_SIFT = 4096;
   const uint32_t ICACHE_SIZE = 0x1000;
   const uint64_t ICACHE_OFFSET_MASK = ICACHE_SIZE - 1;
   /* The key of a code page identified by its physical address: bit 63 set,
      so that it cannot be mistaken for a virtual page in the same caches. */
   const uint64_t CODE_KEY_PHYSICAL = 1ULL << 63;
   const uint64_t ICACHE_PAGE_MASK = ~ICACHE_OFFSET_MASK;

   typedef struct
   {
      uint32_t magic;
      uint32_t size;             //< Size of extra header, in bytes
      uint64_t options;          //< Bit field of Option* options
      // Extra header options
      uint8_t  reserved[];
   } __attribute__ ((__packed__)) Header;

   typedef enum
   {
      CompressionZlib = 1,
      ArchIA32 = 2,
      IcacheVariable = 4,
      PhysicalAddress = 8,
      /* Code is identified by its physical page (CODE_KEY_PHYSICAL), from
         mappings the front end supplies (Writer::Mapping): a full-system
         trace, where processes run different code at the same virtual
         address and one physical page can appear at several. */
      CodePhysical = 16,
   } Option;

   typedef union
   {
      // Simple format for common instructions
      // * first 4 bits: non-zero
      // - address: previous address + previous size
      // - num_addresses: zero or one
      // - is_branch, taken
      // - predicate not supported

      struct {
         uint8_t size:4;    // 1-15
         uint8_t num_addresses:2;
         uint8_t is_branch:1;
         uint8_t taken:1;
         uint64_t addresses[];
      } __attribute__ ((__packed__)) Instruction;

      // Extended format for all instructions
      // * first 4 bits: zero, next 4 bits: non-zero

      struct {
         uint8_t type:4;   // 0
         uint8_t size:4;   // 1-15
         uint8_t num_addresses:2;
         uint8_t is_branch:1;
         uint8_t taken:1;
         uint8_t is_predicate:1;
         uint8_t executed:1;
         uint64_t addr;
         uint64_t addresses[];
      } __attribute__ ((__packed__)) InstructionExt;

      // Other stuff
      // * first 8 bits: zero

      struct {
         uint8_t zero;     // 0
         uint8_t type;
         uint32_t size;
         uint8_t data[];
      } __attribute__ ((__packed__)) Other;

   } __attribute__ ((__packed__)) Record;

   typedef enum {
      RecOtherIcache,
      RecOtherOutput,
      RecOtherSyscallRequest,
      RecOtherSyscallResponse,
      RecOtherNewThread,
      RecOtherNewThreadResponse,
      RecOtherMemoryRequest,
      RecOtherMemoryResponse,
      RecOtherJoin,
      RecOtherJoinResponse,
      RecOtherEndResponse,
      RecOtherIcacheVariable,
      RecOtherSync,
      RecOtherSyncResponse,
      RecOtherLogical2Physical,
      RecOtherMagicInstruction,
      RecOtherMagicInstructionResponse,
      RecOtherRoutineChange,
      RecOtherRoutineAnnounce,
      RecOtherEmu,
      RecOtherEmuResponse,
      RecOtherFork,
      RecOtherForkResponse,
      RecOtherInstructionCount,
      RecOtherCacheOnly,
      RecOtherISAChange,
      RecOtherShutdown,
      RecOtherMemorySpans,
      /* A magic instruction from a front end with no response channel (a
         trace written to a file, a full-system QEMU): the simulator acts on
         it -- an ROI marker switches the performance models -- and does not
         answer, since nothing would read the answer. */
      RecOtherMagicInstructionNoResponse,
      RecOtherEnd = 0xff,
   } RecOtherType;

   typedef enum {
      EmuTypeRdtsc,
      EmuTypeGetProcInfo,
      EmuTypeGetTime,
      EmuTypeCpuid,
      EmuTypeSetThreadInfo,
      EmuTypePAPIstart,
      EmuTypePAPIread,
      EmuTypeCounters,     // the guest's clocks and counters, for a front end that answers its reads of them
      /* The CPU-time clocks: the time the thread ran, the time its process's
         threads ran (not the time they slept or waited), and the clock the
         request names.  Appended: a simulator that does not know it answers
         false, and the front end keeps its own answer. */
      EmuTypeCpuTime,
   } EmuType;

   typedef union {
      struct {
      } rdtsc;
      struct {
      } getprocinfo;
      struct {
      } gettime;
      struct {
         uint32_t eax, ecx;
      } cpuid;
      struct {
         pid_t tid;
      } setthreadinfo;
      struct {
         int eventset;
      } papi;
      struct {
         int32_t clock;           // a CPU-time clock id: 2, 3, or a dynamic (negative) one
      } cputime;
   } EmuRequest;

   typedef union {
      struct {
         uint64_t cycles;
      } rdtsc;
      struct {
         uint64_t procid, nprocs, emunprocs;
      } getprocinfo;
      struct {
         uint64_t time_ns;
      } gettime;
      struct {
         uint64_t time_ns;        // simulated time since the start of the run
         uint64_t core_cycles;    // that time in the core's clock
         uint64_t global_cycles;  // that time in the global clock (as rdtsc's)
         uint64_t instructions;   // instructions the core has run for the thread's application
      } counters;
      struct {
         uint64_t thread_ns;      // the time the thread ran
         uint64_t process_ns;     // the time its process's threads ran, exited ones included
         uint64_t clock_ns;       // the requested clock's time
         uint32_t clock_valid;    // 0: the requested clock names no CPU clock (EINVAL)
      } cputime;
      struct {
         uint32_t eax, ebx, ecx, edx;
      } cpuid;
      struct {
      } setthreadinfo;
      struct {
         long long values[NUM_PAPI_COUNTERS];
      } papi;
   } EmuReply;

   typedef enum {
      ModeUnknown,
      ModeIcount,
      ModeMemory,
      ModeDetailed,
      ModeStop
   } Mode;

   typedef enum {
      CacheOnlyBranchTaken,
      CacheOnlyBranchNotTaken,
      CacheOnlyMemRead,
      CacheOnlyMemWrite,
      CacheOnlyMemIcache,
   } CacheOnlyType;

   // Determine record type based on first uint8_t
   inline bool IsInstructionSimple(uint8_t byte) { return byte > 0; }

};

#endif // __SIFT_FORMAT_H
