#include "magic_server.h"
#include "line_out.h"
#include "sim_api.h"
#include "simulator.h"
#include "thread_manager.h"
#include "logmem.h"
#include "performance_model.h"
#include "instruction.h"
#include "fastforward_performance_model.h"
#include "fastforward_performance_manager.h"
#include "core_manager.h"
#include "dvfs_manager.h"
#include "hooks_manager.h"
#include "stats.h"
#include "timer.h"
#include "thread.h"
#include "trace_manager.h"
#include "trace_thread.h"
#include "mimicos.h"
#include "debug_config.h"
#include "misc/exception_handler_base.h"
#include "sift_utils.h"

MagicServer::MagicServer()
      : m_performance_enabled(false)
{
}

MagicServer::~MagicServer()
{ }

UInt64 MagicServer::Magic(thread_id_t thread_id, core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1, bool *respond)
{
   /* SIM_CMD_WAIT_FOR_ROI blocks until some other thread completes the ROI
      barrier, so it must not be holding the thread manager lock while it
      waits -- the thread it is waiting for needs that same lock to reach
      threadRoiJoin(), and the two would deadlock in exactly the multi-thread
      case the command exists for.  Handled here, before the lock is taken.

      Virtuoso avoids the deadlock by commenting the lock out for every magic
      command; that is a much wider change than this one command needs, so the
      lock stays for everything else. */
   if (cmd == SIM_CMD_WAIT_FOR_ROI)
      return waitForRoi();

   UInt64 res;
   {
      ScopedLock sl(Sim()->getThreadManager()->getLock());
      res = Magic_unlocked(thread_id, core_id, cmd, arg0, arg1, respond);
   }

   return res;
}

/* The L2's accesses (or misses), summed over every core.  The plain name
   ("loads", "load-misses") is the total: the fast cache model registers
   only that, and the cache controller registers it next to its per-block-
   type counters ("loads-data", "load-misses-page_table_data", ...), which
   it is the sum of.  Where only the per-type counters exist (a statistics
   source without the total) they are summed instead; never both, or the
   accesses would be counted twice.  Pac-Sim reads these through
   SIM_CMD_GET_L2_COUNT / SIM_CMD_GET_L2_MISS_COUNT. */
static UInt64 sumCacheMetric(const char *cache, bool misses)
{
   static const char *types[] = { "-page_table_data", "-page_table_instruction", "-instruction", "-data", "-victima_tlb" };
   const char *bases[2] = { misses ? "load-misses" : "loads", misses ? "store-misses" : "stores" };
   UInt64 total = 0;
   for (UInt32 core = 0; core < Sim()->getConfig()->getApplicationCores(); core++)
      for (int b = 0; b < 2; b++)
      {
         StatsMetricBase *m = Sim()->getStatsManager()->getMetricObject(cache, core, bases[b]);
         if (m)
         {
            total += m->recordMetric();
            continue;
         }
         for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); t++)
         {
            m = Sim()->getStatsManager()->getMetricObject(cache, core, String(bases[b]) + types[t]);
            if (m)
               total += m->recordMetric();
         }
      }
   return total;
}

UInt64 MagicServer::Magic_unlocked(thread_id_t thread_id, core_id_t core_id, UInt64 cmd, UInt64 arg0, UInt64 arg1, bool *respond)
{
   switch(cmd)
   {
      case SIM_CMD_ROI_TOGGLE:
         if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         {
            return setPerformance(! m_performance_enabled);
         }
         else
         {
            return 0;
         }
      case SIM_CMD_ROI_START:
         m_legacy_roi_active = true;
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_BEGIN, 0);
         if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         {
            return setPerformance(true);
         }
         else
         {
            return 0;
         }
      case SIM_CMD_ROI_END:
         Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_END, 0);
         if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         {
            return setPerformance(false);
         }
         else
         {
            return 0;
         }
      case SIM_CMD_MHZ_SET:
         return setFrequency(arg0, arg1);
      case SIM_CMD_NAMED_MARKER:
      {
         char str[256];
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         core->accessMemory(Core::NONE, Core::READ, arg1, str, 256, Core::MEM_MODELED_NONE);
         str[255] = '\0';

         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: 0, str: str };
         Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_MARKER, (UInt64)&args);
         return 0;
      }
      case SIM_CMD_SET_THREAD_NAME:
      {
         char str[256];
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         core->accessMemory(Core::NONE, Core::READ, arg0, str, 256, Core::MEM_MODELED_NONE);
         str[255] = '\0';

         Sim()->getStatsManager()->logEvent(StatsManager::EVENT_THREAD_NAME, SubsecondTime::MaxTime(), core_id, thread_id, 0, 0, str);
         Sim()->getThreadManager()->getThreadFromID(thread_id)->setName(str);
         return 0;
      }
      case SIM_CMD_MARKER:
      {
         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_MARKER, (UInt64)&args);
         return 0;
      }
      case SIM_CMD_USER:
      {
         MagicMarkerType args = { thread_id: thread_id, core_id: core_id, arg0: arg0, arg1: arg1, str: NULL };
         return Sim()->getHooksManager()->callHooks(HookType::HOOK_MAGIC_USER, (UInt64)&args, true /* expect return value */);
      }
      case SIM_CMD_INSTRUMENT_MODE:
         return setInstrumentationMode(arg0);
      case SIM_CMD_MHZ_GET:
         return getFrequency(arg0);
      case SIM_CMD_GET_SIM_TIME:
      {
         /* The simulated time so far: the furthest any application core has
            got, in femtoseconds -- the same figure a run reports at its end
            (performance_model.elapsed_time), so that a sum of its deltas over
            the stretches of a run adds up to the run.  Not the barrier's
            global time, which is the slowest core's: while a core that
            started late catches up with the others (the barrier holds them
            for it) the global time advances over a span the others have
            already been through, and the deltas double-count it. */
         UInt64 t = 0;
         for (UInt32 core_i = 0; core_i < Sim()->getConfig()->getApplicationCores(); core_i++)
         {
            Core *core = Sim()->getCoreManager()->getCoreFromID(core_i);
            if (core && core->getPerformanceModel())
            {
               UInt64 ct = core->getPerformanceModel()->getElapsedTime().getFS();
               if (ct > t) t = ct;
            }
         }
         return t;
      }
      case SIM_CMD_GET_INS_NUM:
      {   
          StatsMetricBase * tmp = Sim()->getStatsManager()->getMetricObject("core", arg0,"instructions");
          if(tmp == NULL) {
            return 0;
          } else {
                
            UInt64 ret = tmp->recordMetric();
            return ret;
          }   
      } 
      case SIM_CMD_GET_CORE_TIME:
      {
         /* The calling core's own elapsed time: behind the furthest core's
            by however much this core lags -- a thread that started late,
            or was descheduled -- which is how far it will run before it
            moves the run's time at all. */
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         return (core && core->getPerformanceModel()) ? core->getPerformanceModel()->getElapsedTime().getFS() : 0;
      }
      case SIM_CMD_GET_L2_COUNT:
         return sumCacheMetric("L2", false);
      case SIM_CMD_GET_L2_MISS_COUNT:
         return sumCacheMetric("L2", true);
   case SIM_CMD_GET_BARRIER_REACHED:
      {  
         bool reached = Sim()->getClockSkewMinimizationServer()->onlyMainCoreRunning();
         UInt64 ret = (UInt64)reached;
         Sim()->getClockSkewMinimizationServer()->printState();
         return ret;
      }

      case SIM_CMD_CONTEXT_SWITCH:
      {
#if DEBUG_MAGIC_SERVER >= DEBUG_DETAILED
         LineOut() << "[MimicOS: Magic] Context switch command received" << std::endl;
#endif
         /* Route to the caller's own TraceThread, not thread 0.  A kernel
            worker runs per core -- each created with pthread_create, which
            reaches the simulator as an ordinary guest thread -- so hardcoding
            thread 0 makes every worker switch the first one's reader instead
            of its own.  The second worker then waits forever for a response
            to a switch that happened somewhere else. */
         auto trace_thread = Sim()->getTraceManager()->getTraceThread(0, thread_id);
         if (!trace_thread)
            trace_thread = Sim()->getTraceManager()->getTraceThread(0, 0);

         /* Nothing to switch to: the application this thread was running has
            exited and its reader was cleared.  Installing NULL as the
            current reader crashes the next Read; answering normally lets the
            kernel carry on to its next SimReceiveMessage, where the
            thread_exit it is owed is waiting.  Same shape as the
            CONTEXT_SWITCH_TO miss path. */
         if (!trace_thread->getAppSiftReader())
            return 0;

         // Restore the current SIFT Reader to be the App one
         trace_thread->setCurrentSiftReader(trace_thread->getAppSiftReader());
#if DEBUG_MAGIC_SERVER >= DEBUG_DETAILED
         LineOut() << "[MimicOS: Magic] Current SiftReader set to APP" << std::endl;
#endif
         // The application's stream runs next; the kernel is answered when
         // it is switched back to (TraceThread, sendResponseAfterContextSwitch)
         if (respond)
            *respond = false;
         return 0;
      }
      case SIM_CMD_FAST_FAULT:
      {
         /* Fast-fault replay, first phase.
            The userspace kernel, once its fault profile is trained, stops
            executing the fault-handling work itself and ships the profile
            across instead.  This phase reads the payload and charges the
            profile's mean cost; walking the memory trace through the cache
            hierarchy is a later phase, which is why trace_len and trace_ptr
            are read and reported but not yet followed.

            Payload (see sim_api.h):
              argv[0] marker, argv[1] fp_id, argv[2] frame_pa,
              argv[3] mean_cycles, argv[4] trace_len, argv[5] trace_ptr,
              argv[6] cursor */
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);

         int ff_argc = (int)arg0;
         if (ff_argc < 7)
         {
            std::cerr << "[Virtuoso: SimFastFault] malformed payload, argc="
                      << ff_argc << " (expected >=7)" << std::endl;
            return 0;
         }

         uint64_t ffargv[7] = {0};
         for (int i = 0; i < 7; i++)
         {
            core->accessMemory(Core::NONE, Core::READ,
                               arg1 + i * sizeof(uint64_t),
                               (char*)&ffargv[i], sizeof(uint64_t),
                               Core::MEM_MODELED_NONE);
         }
         const uint64_t fp_id       = ffargv[1];
         const uint64_t frame_pa    = ffargv[2];
         const uint64_t mean_cycles = ffargv[3];
         const uint64_t trace_len   = ffargv[4];
         const uint64_t cursor      = ffargv[6];

         /* Charge the profile's cost to the calling core's timeline, the way
            the DVFS transition latency is charged: a pseudo-instruction the
            performance model accounts for.

            Advancing ShmemPerfModel's elapsed time instead does nothing
            visible.  That clock belongs to the memory subsystem, not to the
            core's instruction timeline, so the cycle count the run reports is
            unmoved by it -- 100 faults of 100,000 cycles each left the total
            at 0.2M cycles either way. */
         if (mean_cycles > 0)
         {
            const ComponentPeriod *period = core->getDvfsDomain();
            PseudoInstruction *delay =
               new DelayInstruction((*period) * mean_cycles,
                                    DelayInstruction::FAST_FAULT);
            core->getPerformanceModel()->queuePseudoInstruction(delay);
         }

         static uint64_t s_fast_fault_count = 0;
         ++s_fast_fault_count;
         std::cerr << "[Virtuoso: SimFastFault] #" << s_fast_fault_count
                   << " core=" << core_id
                   << " fp_id=" << fp_id
                   << " mean_cyc=" << mean_cycles
                   << " frame_pa=0x" << std::hex << frame_pa << std::dec
                   << " trace_len=" << trace_len
                   << " cursor=" << cursor
                   << " argc=" << ff_argc
                   << " payload=0x" << std::hex << arg1 << std::dec
                   << std::endl;
         return 0;
      }
      case SIM_CMD_SET_FAST_FAULT_CHARGE:
      {
         /* The per-fault charge the MMU should queue on the app thread when
            the userspace kernel handles a fault.  Set to the profile's mean
            once it is trained; 0 puts the work back on the kernel. */
         Sim()->getMimicOS()->setFastFaultChargeCycles(arg0);
         std::cerr << "[Virtuoso: SetFastFaultCharge] cycles=" << arg0 << std::endl;
         return 0;
      }
      case SIM_CMD_CONTEXT_SWITCH_TO:
      {
         /* Schedule the user thread named by arg0 onto the calling core.
            SIM_CMD_CONTEXT_SWITCH above can only go back to whatever this
            TraceThread already had as its app reader; this picks a reader
            out of the pool by id, so a kernel can run any injected thread on
            any core. */
         AppReaderPool::app_thread_id_t app_tid = arg0;
         Sift::Reader *target =
            Sim()->getTraceManager()->getAppReaderPool().lookup(app_tid);
         if (!target)
         {
            std::cerr << "[MagicServer] SimContextSwitchTo(" << app_tid
                      << "): no reader in pool; ignoring (thread_id="
                      << thread_id << ", core_id=" << core_id << ")"
                      << std::endl;
            /* Answered, unlike a switch that succeeded (where the caller is
               not resumed and another stream takes over).  Nothing was
               installed here, so the caller's own stream is still current
               and must be answered, or it blocks forever waiting for a
               response that is never sent.  Virtuoso withholds it on this
               path, which strands a
               kernel that asked for a thread that has since exited. */
            return 0;
         }

         auto trace_thread = Sim()->getTraceManager()->getTraceThread(0, thread_id);
         if (!trace_thread)
            trace_thread = Sim()->getTraceManager()->getTraceThread(0, 0);

         /* Re-bind the reader's record handlers to this TraceThread.  The
            reader was bound to whichever TraceThread injected it, and
            handlers must dispatch to the one actually reading the stream --
            otherwise a magic instruction in this stream would be handled on
            the wrong TraceThread. */
         Sim()->getTraceManager()->setTraceReaderHandlers(target, trace_thread);
         trace_thread->setAppSiftReader(target);
         trace_thread->setCurrentSiftReader(target);

         /* Record which user thread this TraceThread is now running, and
            restart its quantum.  Without this the thread runs the
            application but reports no owner, so a quantum_expired event
            names nothing and the kernel has nothing to reschedule. */
         trace_thread->setRunningAppTid(app_tid);

         /* Not answered, like SIM_CMD_CONTEXT_SWITCH: the caller is not meant
            to resume -- the newly installed stream runs next.  That makes
            this a kernel-only command: a guest that calls it for itself is
            never answered and blocks, which is why the check below exercises
            the lookup through the miss path and leaves the install to a
            kernel. */
         Sim()->getMimicOS()->notifyEvent((int)core_id, MimicOS::EVENT_SWITCH_TO, arg0);
         if (respond)
            *respond = false;
         return 0;
      }
      case SIM_CMD_GET_INJECTED_APP_ID:
      {
         /* The pool id of the reader injected into this TraceThread, so a
            kernel can schedule it straight after injecting it.  -1 when
            nothing has been injected here. */
         auto tt = Sim()->getTraceManager()->getTraceThread(0, thread_id);
         if (!tt)
            tt = Sim()->getTraceManager()->getTraceThread(0, 0);
         if (!tt)
            return (UInt64)-1;
         return (UInt64)tt->getInjectedAppId();
      }
      case SIM_CMD_RECEIVE_MESSAGE:
      {
#if DEBUG_MAGIC_SERVER >= DEBUG_DETAILED
         LineOut() << "[MimicOS: Magic] Receive message command" << std::endl;
#endif
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);

         /* Take the next event off this core's queue.  Previously this read a
            single shared slot, which returned the last event posted on any
            core -- and whatever was left there when nothing had been posted
            at all.  An empty queue now delivers a well-formed message with
            argc == 0, so the guest can tell "no event" from a stale one.

            Virtuoso spins here until an event arrives, which it can do
            because it holds no lock.  Magic() holds the thread-manager
            lock, and the posters need it too, so a spin would deadlock;
            the wait below goes through ThreadManager::stallThread() like a
            futex or pause syscall instead, which releases that lock while
            parked and keeps the barrier from waiting on the parked core. */
         MimicOS_NS::Message popped;
         MimicOS_NS::Message *message = &popped;
         MimicOS* mimic = Sim()->getMimicOS();
         ThreadManager* tm = Sim()->getThreadManager();
         while (!mimic->popEvent((int)core_id, popped))
         {
            /* Nothing queued.  If the simulation is winding down, say so
               rather than waiting: a kernel waiting for work has no other
               way to learn that none is coming, and would otherwise sit in
               its dispatch loop until the process was torn down -- taking
               everything it reports at exit with it. */
            if (Sim()->getTraceManager()->isDone())
            {
               MimicOSProtocol::buildMessage(popped, "shutdown");
               break;
            }
            /* Otherwise halt like an idle CPU until a poster wakes us.
               Registration and the stall happen under the lock Magic()
               holds, so a post cannot slip between them; the poster clears
               the registration when it resumes us, and we re-check the
               queue on every wake (a shutdown request also wakes us). */
            SubsecondTime t_halt = core->getPerformanceModel()->getElapsedTime();
            mimic->registerIdleWaiter((int)core_id, thread_id);
            SubsecondTime t_wake = tm->stallThread(thread_id, ThreadManager::STALL_PAUSE, t_halt);
            mimic->clearIdleWaiter((int)core_id);
            if (t_wake > t_halt)
               core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(t_wake, SyncInstruction::PAUSE));
         }

         // Write argc to the core's memory
         core->accessMemory(Core::NONE, Core::WRITE, arg0, (char*)&message->argc, sizeof(int), Core::MEM_MODELED_NONE);

         // Write argv entries
         for (int i = 0; i < message->argc; i++)
         {
            core->accessMemory(Core::NONE, Core::WRITE, arg1 + i * sizeof(uint64_t), (char*)&message->argv[i], sizeof(uint64_t), Core::MEM_MODELED_NONE);
         }
         return 0;
      }
      case SIM_CMD_MIMICOS_RESULT:
      {
#if DEBUG_MAGIC_SERVER >= DEBUG_DETAILED
         LineOut() << "[MimicOS: Magic] MimicOS result command" << std::endl;
#endif
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);

         Sift::Message msg;
         msg.argc = arg0;
         msg.argv = new uint64_t[msg.argc];

         for (int i = 0; i < msg.argc; i++)
         {
            core->accessMemory(Core::NONE, Core::READ, arg1 + i * sizeof(uint64_t), (char*)&msg.argv[i], sizeof(uint64_t));
         }

         int exception_type_code = msg.argv[0];
         core->getExceptionHandler()->handle_exception(exception_type_code, msg.argc, msg.argv);

         delete[] msg.argv;
         return 0;
      }
      case SIM_CMD_START_PROCESS:
      {
         char str[256];
         Core *core = Sim()->getCoreManager()->getCoreFromID(core_id);
         core->accessMemory(Core::NONE, Core::READ, arg0, str, 256, Core::MEM_MODELED_NONE);
         str[255] = '\0';

#if DEBUG_MAGIC_SERVER >= DEBUG_DETAILED
         LineOut() << "[MimicOS: Magic] Starting process: " << str << std::endl;
#endif
         Sim()->getTraceManager()->createTraceBasedApplication(SubsecondTime::Zero(), str, thread_id);
         return 0;
      }
      case SIM_CMD_MALLOC:
      {
         // Placeholder for malloc tracking
         return 0;
      }

      case SIM_CMD_VECTOR_BYTES:
      {
         /* The width the guest's vector registers have (the QEMU plugin
            reads vlenb or SVE's vg) against the width this configuration
            times them at; they used to match only by convention */
         const String arch = Sim()->getCfg()->getString("general/arch");
         const char *key = arch == "riscv" ? "perf_model/core/riscv/vector_bytes"
                         : (arch == "armv8" || arch == "arm") ? "perf_model/core/arm/sve_bytes" : NULL;
         if (key && Sim()->getCfg()->hasKey(key))
         {
            const UInt64 modelled = Sim()->getCfg()->getIntArray(key, core_id == INVALID_CORE_ID ? 0 : core_id);
            if (modelled != arg0)
               LOG_PRINT_ERROR("The guest's vector registers are %" PRIu64 " bytes (%" PRIu64 " bits) and the configuration "
                               "models %" PRIu64 " (%s): record with that width (QEMU -cpu ...,vlen=%" PRIu64 " for RISC-V, "
                               "sve-default-vector-length=%" PRIu64 " for AArch64), or set %s=%" PRIu64 ".",
                               arg0, arg0 * 8, modelled, key, modelled * 8, modelled, key, arg0);
         }
         return 0;
      }
      case SIM_CMD_ROI_EXPECT:
         return roiExpect(arg0);
      case SIM_CMD_THREAD_ROI_START:
         /* arg0 is a caller-chosen participant handle, not a thread_id: a
            single Sniper TraceThread can carry several live processes under
            the inject-forked-app model, and a PID would drift under syscall
            emulation. */
         return threadRoiJoin((thread_id_t)arg0);
      case SIM_CMD_THREAD_ROI_END:
         return threadRoiLeave((thread_id_t)arg0);
      case SIM_CMD_WAIT_FOR_ROI:
         /* Guests never arrive here: Magic() intercepts this command before
            taking the thread manager lock, precisely so the wait does not
            hold it.  This branch is reachable only from the scripting entry
            points, which call Magic_unlocked() directly and may already hold
            the lock, so it answers without blocking. */
         return roiReady() ? 0 : 1;

      // K1: the magic protocol numbers are reserved here ahead of the
      // handlers, so a guest built against the new sim_api.h runs against
      // this build instead of tripping the assert in `default:` below.
      // Each is filled in by a later step: the ROI barrier by K2, the
      // scheduling pair by K5, the fast-fault pair by K6.
      {
         return 0;
      }
      default:
         LOG_ASSERT_ERROR(false, "Got invalid Magic %lu, arg0(%lu) arg1(%lu)", cmd, arg0, arg1);
   }
   return 0;
}

UInt64 MagicServer::getGlobalInstructionCount(void)
{
   UInt64 ninstrs = 0;
   for (UInt32 i = 0; i < Sim()->getConfig()->getApplicationCores(); i++)
      ninstrs += Sim()->getCoreManager()->getCoreFromID(i)->getInstructionCount();
   return ninstrs;
}

static Timer t_start;
UInt64 ninstrs_start;
__attribute__((weak)) void PinDetach(void) {}

void MagicServer::enablePerformance()
{
   /* The models first, the snapshot after: leaving fast-forward brings the
      barrier's global time up to the cores (it lagged them by up to a
      fast-forward quantum, 100 us by default), and the run's reported
      elapsed time is that global time's delta between the ROI's begin and
      end.  Snapshotted before the transition, a short ROI read up to a
      quantum long. */
   Simulator::enablePerformanceModels();
   Sim()->setInstrumentationMode(InstMode::inst_mode_roi, true /* update_barrier */);

   Sim()->getStatsManager()->recordStats("roi-begin");
   ninstrs_start = getGlobalInstructionCount();
   t_start.start();
}

void MagicServer::disablePerformance()
{
   Simulator::disablePerformanceModels();
   Sim()->getStatsManager()->recordStats("roi-end");

   float seconds = t_start.getTime() / 1e9;
   UInt64 ninstrs = getGlobalInstructionCount() - ninstrs_start;
   double simtime = Sim()->getClockSkewMinimizationServer()->getGlobalTime().getNS() * 1e-9;
   UInt64 cycles = SubsecondTime::divideRounded(Sim()->getClockSkewMinimizationServer()->getGlobalTime(),
                                                Sim()->getCoreManager()->getCoreFromID(0)->getDvfsDomain()->getPeriod());
   printf("[SNIPER] Simulated %.1fM instructions, %.1fM cycles, %.2f IPC\n",
      ninstrs / 1e6,
      cycles / 1e6,
      float(ninstrs) / (cycles ? cycles : 1));
   printf("[SNIPER] Simulation Time %.3f\n",simtime);
   printf("[SNIPER] Simulation speed %.1f KIPS (%.1f KIPS / target core - %.1fns/instr)\n",
      ninstrs / seconds / 1e3,
      ninstrs / seconds / 1e3 / Sim()->getConfig()->getApplicationCores(),
      seconds * 1e9 / (float(ninstrs ? ninstrs : 1.) / Sim()->getConfig()->getApplicationCores()));

   PerformanceModel *perf = Sim()->getCoreManager()->getCoreFromID(0)->getPerformanceModel();
   if (perf->getFastforwardPerformanceModel()->getFastforwardedTime() > SubsecondTime::Zero())
   {
      // NOTE: Prints out the non-idle ratio for core 0 only, but it's just indicative anyway
      double ff_ratio = double(perf->getFastforwardPerformanceModel()->getFastforwardedTime().getNS())
                      / double(perf->getNonIdleElapsedTime().getNS());
      double percent_detailed = 100. * (1. - ff_ratio);
      printf("[SNIPER] Sampling: executed %.2f%% of simulated time in detailed mode\n", percent_detailed);
   }

   fflush(NULL);

   Sim()->setInstrumentationMode(InstMode::inst_mode_end, true /* update_barrier */);
   PinDetach();
}

void print_allocations();

UInt64 MagicServer::setPerformance(bool enabled)
{
   if (m_performance_enabled == enabled)
      return 1;

   m_performance_enabled = enabled;

   /* Publish readiness for SimWaitForRoi.  Virtuoso sets this only in
      threadRoiJoin(), which leaves a waiter spinning forever when ROI was
      opened by the legacy SimRoiStart path -- despite WAIT_FOR_ROI being
      documented as waiting for either.  Setting it here covers both. */
   m_roi_ready.store(enabled, std::memory_order_release);

   //static bool enabled = false;
   static Timer t_start;
   //ScopedLock sl(l_alloc);

   if (m_performance_enabled)
   {
      printf("[SNIPER] Enabling performance models\n");
      fflush(NULL);
      t_start.start();
      logmem_enable(true);
      Sim()->getHooksManager()->callHooks(HookType::HOOK_ROI_BEGIN, 0);
   }
   else
   {
      Sim()->getHooksManager()->callHooks(HookType::HOOK_ROI_END, 0);
      printf("[SNIPER] Disabling performance models\n");
      float seconds = t_start.getTime() / 1e9;
      printf("[SNIPER] Leaving ROI after %.2f seconds\n", seconds);
      fflush(NULL);
      logmem_enable(false);
      logmem_write_allocations();
   }

   if (enabled)
      enablePerformance();
   else
      disablePerformance();

   return 0;
}

/* Per-thread ROI barrier (Apr 15 2026).

   The coordinator announces how many threads will take part; each thread then
   joins and leaves from its own context.  Performance models turn on exactly
   when the last expected joiner arrives, and off when the last joiner leaves.
   That is what the single-threaded SimRoiStart/SimRoiEnd pair cannot express:
   with several threads it opens ROI on whichever thread got there first. */
UInt64 MagicServer::roiExpect(UInt64 n)
{
   std::lock_guard<std::mutex> lk(m_roi_lock);
   /* Raising the count is allowed -- a live app may be spawned late -- but not
      shrinking it below the threads that have already joined, which would
      strand them. */
   if (n < m_roi_joined.size())
   {
      std::cerr << "[MagicServer] SimRoiExpect(" << n << ") rejected: "
                << m_roi_joined.size() << " threads already joined" << std::endl;
      return 1;
   }
   m_roi_expected = n;
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
   printf("[SNIPER] ROI barrier: expecting %zu threads\n", m_roi_expected);
   fflush(NULL);
#endif
   /* Nothing happens to the performance models until the last joiner lands. */
   return 0;
}

UInt64 MagicServer::threadRoiJoin(thread_id_t thread_id)
{
   bool should_enable = false;
   {
      std::lock_guard<std::mutex> lk(m_roi_lock);
      m_roi_joined.insert(thread_id);
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
      printf("[SNIPER] ROI join: thread %d (%zu/%zu)\n",
             thread_id, m_roi_joined.size(), m_roi_expected);
      fflush(NULL);
#endif
      /* Only act when ROI is actually under magic control.  Under ROI_FULL
         the performance models are already on for the whole application and
         the barrier has nothing to gate, exactly as SIM_CMD_ROI_START is
         inert there.  The bookkeeping above still runs, so a guest that uses
         the barrier reads the same counts either way. */
      if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC
          && m_roi_expected > 0 && m_roi_joined.size() >= m_roi_expected
          && !m_performance_enabled)
      {
         should_enable = true;
      }
   }
   /* Outside the lock: the hooks and setPerformance run arbitrary code. */
   if (should_enable)
   {
      Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_BEGIN, 0);
      if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         setPerformance(true);
      /* Release any thread spinning in SimWaitForRoi.  setPerformance has
         already done this when ROI_MAGIC is in force; this covers the case
         where it is not, where the barrier is still the event being waited
         for even though no performance model changes state. */
      m_roi_ready.store(true, std::memory_order_release);
   }
   return 0;
}

UInt64 MagicServer::threadRoiLeave(thread_id_t thread_id)
{
   bool should_disable = false;
   {
      std::lock_guard<std::mutex> lk(m_roi_lock);
      m_roi_joined.erase(thread_id);
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
      printf("[SNIPER] ROI leave: thread %d (%zu/%zu)\n",
             thread_id, m_roi_joined.size(), m_roi_expected);
      fflush(NULL);
#endif
      /* Same gate as the join side, and it matters more here: this path
         stops the TraceManager.  Under ROI_FULL the models are enabled from
         the start, so an unguarded check would see "joined set empty and
         performance on" the first time any guest called SimThreadRoiEnd and
         tear the run down mid-application. */
      if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC
          && m_roi_joined.empty() && m_performance_enabled)
      {
         should_disable = true;
      }
   }
   if (should_disable)
   {
      Sim()->getHooksManager()->callHooks(HookType::HOOK_APPLICATION_ROI_END, 0);
      if (Sim()->getConfig()->getSimulationROI() == Config::ROI_MAGIC)
         setPerformance(false);
      /* The last barrier participant has left.  Stop the TraceManager only if
         no trace-based application is still running: in mixed mode the legacy
         SimRoiStart path owns shutdown, and stopping here would cut those
         applications short. */
      /* The last barrier participant has left, so the run is over.  Stop only
         when no trace-based application is still going: in mixed mode the
         legacy SimRoiStart path owns shutdown, and stopping here would cut
         those applications short.

         This was removed when it first landed, because stopping from here
         raced a spawned thread that was still exiting and killed about one
         run in three in StatsManager::logEvent.  What made it safe is the
         shutdown machinery this now depends on: stop() enters once and tells
         every registered reader's front end to stop, so a recorder waiting
         on a response that will never arrive exits instead of failing on a
         short read.  Measured over 26 runs after that landed, against 7 of
         10 before it. */
      if (!m_legacy_roi_active)
      {
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
         printf("[SNIPER] Last app left ROI; stopping TraceManager\n");
         fflush(NULL);
#endif
         Sim()->getTraceManager()->stop();
      }
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
      else
      {
         printf("[SNIPER] Last live app left ROI (trace apps still active)\n");
         fflush(NULL);
      }
#endif
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
      else
      {
         printf("[SNIPER] Last live app left ROI (trace apps still active)\n");
         fflush(NULL);
      }
#endif
   }
   return 0;
}

/* Block until the ROI barrier has opened.  Called from Magic() before the
   thread manager lock is taken -- see the comment there. */
UInt64 MagicServer::waitForRoi()
{
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
   printf("[SNIPER] WaitForRoi: waiting for perf model to enable...\n");
   fflush(NULL);
#endif
   while (!m_roi_ready.load(std::memory_order_acquire))
   {
      usleep(1000);  // 1ms poll; the waiting core has nothing else to do
   }
#if DEBUG_MAGIC_SERVER >= DEBUG_BASIC
   printf("[SNIPER] WaitForRoi: perf model is on, proceeding\n");
   fflush(NULL);
#endif
   return 0;
}

UInt64 MagicServer::setFrequency(UInt64 core_number, UInt64 freq_in_mhz)
{
   UInt32 num_cores = Sim()->getConfig()->getApplicationCores();
   UInt64 freq_in_hz;
   if (core_number >= num_cores)
      return 1;
   freq_in_hz = 1000000 * freq_in_mhz;

   printf("[SNIPER] Setting frequency for core %" PRId64 " in DVFS domain %d to %" PRId64 " MHz\n", core_number, Sim()->getDvfsManager()->getCoreDomainId(core_number), freq_in_mhz);

   if (freq_in_hz > 0)
      Sim()->getDvfsManager()->setCoreDomain(core_number, ComponentPeriod::fromFreqHz(freq_in_hz));
   else {
      Sim()->getThreadManager()->stallThread_async(core_number, ThreadManager::STALL_BROKEN, SubsecondTime::MaxTime());
      Sim()->getCoreManager()->getCoreFromID(core_number)->setState(Core::BROKEN);
   }

   // First set frequency, then call hooks so hook script can find the new frequency by querying the DVFS manager
   Sim()->getHooksManager()->callHooks(HookType::HOOK_CPUFREQ_CHANGE, core_number);

   return 0;
}

UInt64 MagicServer::getFrequency(UInt64 core_number)
{
   UInt32 num_cores = Sim()->getConfig()->getApplicationCores();
   if (core_number >= num_cores)
      return UINT64_MAX;

   const ComponentPeriod *per = Sim()->getDvfsManager()->getCoreDomain(core_number);
   return per->getPeriodInFreqMHz();
}

UInt64 MagicServer::setInstrumentationMode(UInt64 sim_api_opt)
{
   InstMode::inst_mode_t inst_mode;
   switch (sim_api_opt)
   {
   case SIM_OPT_INSTRUMENT_DETAILED:
      inst_mode = InstMode::DETAILED;
      break;
   case SIM_OPT_INSTRUMENT_WARMUP:
      inst_mode = InstMode::CACHE_ONLY;
      break;
   case SIM_OPT_INSTRUMENT_FASTFORWARD:
      inst_mode = InstMode::FAST_FORWARD;
      break;
   default:
      LOG_PRINT_ERROR("Unexpected magic instrument opt type: %lx.", sim_api_opt);
   }
   /* The instrumentation mode alone is only half of a switch.  What the
      trace thread does with each record is one state; whether each core's
      performance model is in fast-forward, whether the fast-forward manager
      is running and whether the barrier is in fast-forward mode is another,
      and the two are moved together only on the ROI path
      (MagicServer::enablePerformance / disablePerformance).  Switched on its
      own inside the ROI, a fast-forward mode left the manager disabled and
      the barrier in detailed mode: no core's clock advanced (the detailed
      model drops instruction counts unless it is fast-forwarding), cores
      already waiting in the barrier stayed parked, and the one thread whose
      trace thread never enters the barrier in that mode ran a whole region
      alone (Pac-Sim's loop shape).  So the same pair the ROI toggle uses
      moves the second state here too, when it has not been moved already. */
   if (m_performance_enabled && Sim()->getFastForwardPerformanceManager())
   {
      const bool want_fastforward = inst_mode != InstMode::DETAILED;
      if (want_fastforward && !Sim()->getFastForwardPerformanceManager()->isEnabled())
         Simulator::disablePerformanceModels();
      else if (!want_fastforward && Sim()->getFastForwardPerformanceManager()->isEnabled())
         Simulator::enablePerformanceModels();
   }
   Sim()->setInstrumentationMode(inst_mode, true /* update_barrier */);

   return 0;
}
