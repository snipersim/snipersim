#include "windows.h"
#include "log.h"
#include "instruction.h"
#include "lll_info.h"

#include <algorithm>

#include <sstream>
#include <iostream>
#include <iomanip>
#include <assert.h>

// For validation of the functional unit contention code, uncomment below at the cost of simulation speed.
//#define DEBUG_WINDOW_CONTENTION_CHECK

Windows::Windows(int window_size, bool doFunctionalUnitContention)
   : m_double_window(new MicroOp[2*window_size])
   , m_exec_time_map(new uint32_t[2*window_size])
   , m_do_functional_unit_contention(doFunctionalUnitContention)
   , m_register_dependencies(new RegisterDependencies())
   , m_memory_dependencies(new MemoryDependencies())
{
   m_window_size = window_size;
   m_double_window_size = 2*window_size;

   for(int i = 0; i < m_double_window_size; i++)
   {
      m_double_window[i].setWindowIndex(i);
   }

   clear();
   clearFunctionalUnitStats();
}

Windows::~Windows()
{
   delete[] m_double_window;
   delete[] m_exec_time_map;
   delete m_register_dependencies;
   delete m_memory_dependencies;
}

void Windows::clear()
{
   for (int i = 0; i < m_double_window_size; i++)
   {
      m_exec_time_map[i] = 0;
   }

   m_next_sequence_number = 0;

   m_register_dependencies->clear();
   m_memory_dependencies->clear();

   m_window_head__old_window_tail = m_window_tail = m_old_window_head = 0;
   m_window_length = m_old_window_length = 0;
   m_critical_path_head = m_critical_path_tail = 0;
   clearFunctionalUnitStats();
}

void Windows::clearFunctionalUnitStats()
{
   for(unsigned int i = 0; i < (unsigned int)MicroOp::UOP_TYPE_SIZE; ++i)
   {
      m_count_bytype[i] = 0;
   }
   for(unsigned int i = 0; i < (unsigned int)CPCONTR_TYPE_SIZE; ++i)
   {
      m_cpcontr_bytype[i] = 0;
   }
   m_cpcontr_total = 0;
}

void Windows::addFunctionalUnitStats(const MicroOp &uop)
{
   m_count_bytype[uop.getUopType()]++;
   m_cpcontr_bytype[getCpContrType(uop)] += uop.getCpContr();
   m_cpcontr_total += uop.getCpContr();
}

void Windows::removeFunctionalUnitStats(const MicroOp &uop)
{
   m_count_bytype[uop.getUopType()]--;
   m_cpcontr_bytype[getCpContrType(uop)] -= uop.getCpContr();
   m_cpcontr_total -= uop.getCpContr();
}

bool Windows::wIsFull() const
{
   return m_window_length == m_window_size;
}

bool Windows::wIsEmpty() const
{
   return m_window_length == 0;
}

int Windows::incrementIndex(const int index) const
{
   return (index + 1) % m_double_window_size;
}

int Windows::decrementIndex(const int index) const
{
   return (index + m_double_window_size - 1) % m_double_window_size;
}

int Windows::windowIndex(const int index) const
{
   return ((index % m_double_window_size) + m_double_window_size) % m_double_window_size;
}

/**
 * Add the microOperation to the window and calculate its dependencies.
 */
void Windows::add(const MicroOp& micro_op)
{
   LOG_ASSERT_ERROR(!wIsFull(), "Window is full");

   MicroOp& windowMicroOp = getInstructionByIndex(m_window_tail);
   m_window_tail = incrementIndex(m_window_tail);
   m_window_length++;
   micro_op.copyTo(windowMicroOp);
   windowMicroOp.setSequenceNumber(m_next_sequence_number);
   m_next_sequence_number++;

   uint64_t lowestValidSequenceNumber = getInstructionByIndex(m_old_window_head).getSequenceNumber();
   m_register_dependencies->setDependencies(windowMicroOp, lowestValidSequenceNumber);
   m_memory_dependencies->setDependencies(windowMicroOp, lowestValidSequenceNumber);
}

MicroOp& Windows::getInstructionByIndex(int index) const
{
   LOG_ASSERT_ERROR(index >= 0 && index < m_double_window_size, "Index is out of bounds");
   return m_double_window[index];
}

MicroOp& Windows::getInstruction(uint64_t sequenceNumber) const
{
   MicroOp& windowHead = getInstructionByIndex(m_window_head__old_window_tail);
   int distance = windowHead.getSequenceNumber() - sequenceNumber;
   int index = windowIndex(windowHead.getWindowIndex() - distance);

   MicroOp& ret = getInstructionByIndex(index);
   if (ret.getSequenceNumber() == sequenceNumber)
   {
      return ret;
   }
   else
   {
      std::cout << "Getting an instruction that is not in the window !!! That's it, I'm out of here !" << std::endl;
      exit(0);
   }
}

MicroOp& Windows::getLastAdded() const
{
   return getInstructionByIndex(decrementIndex(m_window_tail));
}

MicroOp& Windows::getInstructionToDispatch() const
{
   return getInstructionByIndex(m_window_head__old_window_tail);
}

MicroOp& Windows::getOldestInstruction() const
{
   return getInstructionByIndex(m_old_window_head);
}

WindowStopDispatchReason addWSDR(WindowStopDispatchReason orig, WindowStopDispatchReason to_add)
{
   return WindowStopDispatchReason(int(orig) + int(to_add));
}

WindowStopDispatchReason Windows::shouldStopDispatch()
{

   WindowStopDispatchReason r = WIN_STOP_DISPATCH_NO_REASON;

#ifdef DEBUG_WINDOW_CONTENTION_CHECK
   // Normally, we keep track of this information when microOps are added and removed
   // from the old window.  Here, we scan over the old window for each request, with
   // about a 20 to 30% slowdown.  Additionally, we double-check that our faster methods
   // are correct with assertions.
   uint64_t count_bytype[UOP_TYPE_SIZE] = { 0 };

   // Count all of the ADD/SUB/DIV/LD/ST, and if we have too many, break
   // Count all of the GENERIC insns, and if we have too many (3x-per-cycle), break
   Windows::Iterator i = getOldWindowIterator();
   while(i.hasNext())
   {
      MicroOp &micro_op = i.next();
      count_bytype[micro_op.getUopType()]++;
   }

   LOG_ASSERT_ERROR(count_bytype[MicroOp::UOP_TYPE_LOAD] == m_count_bytype[MicroOp::UOP_TYPE_LOAD], "Count Load %ld != %ld", count_bytype[MicroOp::UOP_TYPE_LOAD], m_count_bytype[MicroOp::UOP_TYPE_LOAD]);
   LOG_ASSERT_ERROR(count_bytype[MicroOp::UOP_TYPE_STORE] == m_count_bytype[MicroOp::UOP_TYPE_STORE], "Count Store %ld != %ld", count_bytype[MicroOp::UOP_TYPE_STORE], m_count_bytype[MicroOp::UOP_TYPE_STORE]);
   LOG_ASSERT_ERROR(count_bytype[MicroOp::UOP_TYPE_FP_ADDSUB] == m_count_bytype[MicroOp::UOP_TYPE_FP_ADDSUB], "Count FP Add/Sub mismatch %ld != %ld", count_bytype[MicroOp::UOP_TYPE_FP_ADDSUB], m_count_bytype[MicroOp::UOP_TYPE_FP_ADDSUB]);
   LOG_ASSERT_ERROR(count_bytype[MicroOp::UOP_TYPE_FP_MULDIV] == m_count_bytype[MicroOp::UOP_TYPE_FP_MULDIV], "Count FP Mul/Div mismatch %ld != %ld", count_bytype[MicroOp::UOP_TYPE_FP_MULDIV], m_count_bytype[MicroOp::UOP_TYPE_FP_MULDIV]);
   LOG_ASSERT_ERROR(count_bytype[MicroOp::UOP_TYPE_GENERIC] == m_count_bytype[MicroOp::UOP_TYPE_GENERIC], "Count Generic mismatch %ld != %ld", count_bytype[MicroOp::UOP_TYPE_GENERIC], m_count_bytype[MicroOp::UOP_TYPE_GENERIC]);
   LOG_ASSERT_ERROR(count_bytype[MicroOp::UOP_TYPE_BRANCH] == m_count_bytype[MicroOp::UOP_TYPE_BRANCH], "Count Branch mismatch %ld != %ld", count_bytype[MicroOp::UOP_TYPE_BRANCH], m_count_bytype[MicroOp::UOP_TYPE_BRANCH]);
#endif /* DEBUG_WINDOW_CONTENTION_CHECK */

   uint64_t critical_path_length = getCriticalPathLength();

   if (m_count_bytype[MicroOp::UOP_TYPE_LOAD] > critical_path_length)       // port 2
      r = addWSDR(r, WIN_STOP_DISPATCH_LOAD);
   if (m_count_bytype[MicroOp::UOP_TYPE_STORE] > critical_path_length)      // port 3+4
      r = addWSDR(r, WIN_STOP_DISPATCH_STORE);
   if (m_count_bytype[MicroOp::UOP_TYPE_FP_ADDSUB] > critical_path_length)  // port 1
      r = addWSDR(r, WIN_STOP_DISPATCH_FP_ADDSUB);
   if (m_count_bytype[MicroOp::UOP_TYPE_FP_MULDIV] > critical_path_length)  // port 0
      r = addWSDR(r, WIN_STOP_DISPATCH_FP_MULDIV);
   if (m_count_bytype[MicroOp::UOP_TYPE_BRANCH] > critical_path_length)     // port 5
      r = addWSDR(r, WIN_STOP_DISPATCH_BRANCH);
   if ((m_count_bytype[MicroOp::UOP_TYPE_FP_ADDSUB]+m_count_bytype[MicroOp::UOP_TYPE_FP_MULDIV]+m_count_bytype[MicroOp::UOP_TYPE_BRANCH]
         +m_count_bytype[MicroOp::UOP_TYPE_GENERIC]) > (3*critical_path_length)) // port 0, 1 or 5
      r = addWSDR(r, WIN_STOP_DISPATCH_GENERIC);

   return r;
}

WindowStopDispatchReason Windows::dispatchInstruction()
{
   if (m_old_window_length == m_window_size)
   {
      // No longer keep track of instructions leaving the old window
      removeFunctionalUnitStats(getInstructionByIndex(m_old_window_head));

      /* Recalculate critical path: new m_old_window_head */
      m_critical_path_head = std::max(m_critical_path_head, getInstructionByIndex(m_old_window_head).getExecTime());
      m_old_window_head = incrementIndex(m_old_window_head);
      m_old_window_length--;
   }

   // Keep track of the just dispatched instruction
   // Determine if this new instruction will cause the use of too many resources
   addFunctionalUnitStats(getInstructionByIndex(m_window_head__old_window_tail));

   m_window_head__old_window_tail = incrementIndex(m_window_head__old_window_tail);
   m_window_length--;
   m_old_window_length++;

   if (m_do_functional_unit_contention)
   {
      return shouldStopDispatch();
   }
   else
   {
      return WIN_STOP_DISPATCH_NO_REASON;
   }
}

void Windows::clearOldWindow(uint64_t newCpHead)
{
   m_critical_path_head = m_critical_path_tail = newCpHead;
   m_old_window_length = 0;
   m_old_window_head = m_window_head__old_window_tail;

   clearFunctionalUnitStats();
}

bool Windows::windowContains(uint64_t sequenceNumber) const
{
   uint64_t lowestValid = getInstructionByIndex(m_window_head__old_window_tail).getSequenceNumber();
   uint64_t highestValid = getInstructionByIndex(decrementIndex(m_window_tail)).getSequenceNumber();
   return sequenceNumber >= lowestValid && sequenceNumber <= highestValid;
}

bool Windows::oldWindowContains(uint64_t sequenceNumber) const
{
   uint64_t lowestValid = getInstructionByIndex(m_old_window_head).getSequenceNumber();
   uint64_t highestValid = getInstructionByIndex(decrementIndex(m_window_head__old_window_tail)).getSequenceNumber();
   return sequenceNumber >= lowestValid && sequenceNumber <= highestValid;
}

int Windows::getOldWindowLength() const
{
   return m_old_window_length;
}

uint64_t Windows::getCriticalPathHead() const
{
   return m_critical_path_head;
}

uint64_t Windows::getCriticalPathTail() const
{
   return m_critical_path_tail;
}

int Windows::getCriticalPathLength() const
{
   if ( ! (m_critical_path_head <= m_critical_path_tail) )
   {
      LOG_PRINT_WARNING_ONCE("Warning: Windows::getCriticalPathLength() - head:%lu > tail:%lu", m_critical_path_head, m_critical_path_tail);
      return 0;
   }
   return (m_critical_path_tail - m_critical_path_head);
}

uint64_t Windows::longLatencyOperationLatency(MicroOp& uop)
{
   if (uop.getExecTime() < m_critical_path_tail)
   {
      // No critical path extension
      return 0;
   }
   else if (uop.getExecTime() - m_critical_path_tail < lll_info.getCutoff())
   {
      // Extension smaller than LLL cutoff
      return 0;
   }
   else
   {
      // Critical path extension longer than LLL cutoff
      return true;
   }
}

uint64_t Windows::updateCriticalPathTail(MicroOp& uop)
{
   uint64_t execTime = uop.getExecTime();
   if (execTime > m_critical_path_tail) {
      if (execTime - m_critical_path_tail > lll_info.getCutoff()) {
         // Extending the critical path by huge amounts isn't healthy (see Redmine #113).
         // Instead, the caller should add the long latency directly (and attribute it) and call clearOldWindow()
         LOG_PRINT_WARNING_ONCE("Warning: Updating the critical path by %lu > %u cycles", execTime - m_critical_path_tail, lll_info.getCutoff());
      }
      uop.setCpContr(execTime - m_critical_path_tail);
      m_critical_path_tail = execTime;
   }
   #if 0
      // IPC trace per X instructions
      const int uop_seq_increment = 5000;
      static int uop_seq_next = 1000;
      if (uop.getSequenceNumber() > uop_seq_next) {
         FixedPoint ipc = FixedPoint(getOldWindowLength()) / getCriticalPathLength();
         printf("IPC [wl=%3u,cpl=%3u] ipKc %4u\n", getOldWindowLength(), getCriticalPathLength(), FixedPoint::floor(ipc*1000));
         uop_seq_next += uop_seq_increment;
      }
   #endif
   #if 0
      // All instructions and dependencies with instantaneous IPC
      const int uop_seq_start = 100000, uop_seq_length = 5000;
      if (uop.getSequenceNumber() > uop_seq_start && uop.getSequenceNumber() < uop_seq_start + uop_seq_length) {
         FixedPoint ipc = FixedPoint(getOldWindowLength()) / getCriticalPathLength();
         printf("[wl=%3u,cpl=%3u] uop s=%5u el=%3u et=%6u   [ipKc %4u] %-8s %-30s   %2u %u dep=",
            getOldWindowLength(), getCriticalPathLength(), uop.getSequenceNumber(),
            uop.getExecLatency(), execTime, FixedPoint::floor(ipc*1000),
            uop.instructionOpcodeName.c_str(),
            uop.getInstruction() ? uop.getInstruction()->getDisassembly().c_str() : "(dynamic)",
            uop.getInstructionType(), uop.isX87() ? 1 : 0);
         for(uint32_t i = 0; i < uop.getDependenciesLength(); i++) {
            if (oldWindowContains(uop.getDependency(i))) {
               MicroOp& dependee = getInstruction(uop.getDependency(i));
               printf(" %5u", dependee.getSequenceNumber());
            }
         }
         printf("\n");
      }
   #endif
   return m_critical_path_tail;
}

int Windows::getMinimalFlushLatency(int width) const
{
   return (m_old_window_length+(width-1))/width;
}

uint64_t Windows::getCpContrFraction(CpContrType type) const
{
   assert(m_cpcontr_bytype[type] <= m_cpcontr_total);
   if (m_cpcontr_total > 0)
      return 1000000 * m_cpcontr_bytype[type] / m_cpcontr_total;
   else
      return 0;
}

Windows::Iterator::Iterator(const Windows* const windows, int start, int stop): index(start), stop(stop), windows(windows) { }

bool Windows::Iterator::hasNext()
{
   return index != stop;
}

MicroOp& Windows::Iterator::next()
{
   MicroOp& ret = windows->getInstructionByIndex(index);
   index = windows->incrementIndex(index);
   return ret;
}

Windows::Iterator Windows::getWindowIterator() const
{
   return Iterator(this, m_window_head__old_window_tail, m_window_tail);
}

Windows::Iterator Windows::getOldWindowIterator() const
{
   return Iterator(this, m_old_window_head, m_window_head__old_window_tail);
}

int Windows::calculateBranchResolutionLatency()
{
   MicroOp& micro_op = getInstructionToDispatch();
   uint32_t br_resolution_latency = 0;

   // The m_exec_time_map is empty when the algorithm starts !

   // Mark direct producers of this instruction
   for(uint32_t i = 0; i < micro_op.getDependenciesLength(); i++)
   {
      if (oldWindowContains(micro_op.getDependency(i)))
      {
         MicroOp& producer = getInstruction(micro_op.getDependency(i));
         m_exec_time_map[producer.getWindowIndex()] = producer.getExecLatency();
      }
   }

   // Find/mark producers of producers
   for (int i = windowIndex(m_window_head__old_window_tail - 1), j = 0; j < m_old_window_length; i = windowIndex(i - 1), j++)
   {
      if (m_exec_time_map[i])
      {
         // There is a path to the committed branch: check the dependencies
         MicroOp& op = getInstructionByIndex(i);
         for (uint32_t k = 0; k < op.getDependenciesLength(); k++)
         {
            if (oldWindowContains(op.getDependency(k)))
            {
               MicroOp& producer = getInstruction(op.getDependency(k));
               m_exec_time_map[producer.getWindowIndex()] = std::max((producer.getExecLatency() + m_exec_time_map[i]), m_exec_time_map[producer.getWindowIndex()]);
            }
         }

         br_resolution_latency = std::max(br_resolution_latency, m_exec_time_map[i]);
         // Reset m_exec_time_map entry
         m_exec_time_map[i] = 0;
      }
   }

   return br_resolution_latency;
}

String Windows::toString()
{
   std::ostringstream out;
   out << "|";
   for(int i = 0; i < m_double_window_size; i++)
   {
      if (i == m_old_window_head)
         out << "OH";
      if (i == m_window_head__old_window_tail)
         out << "WH";
      if (i == m_window_tail)
         out << "WT";
      out << std::setfill(' ') << std::setw(5) << getInstructionByIndex(i).getWindowIndex() << "|";
   }
   out << std::endl << "|";
   for(int i = 0; i < m_double_window_size; i++)
      out << std::setfill(' ') << std::setw(7) << getInstructionByIndex(i).getSequenceNumber() << "|";
   out << std::endl;

   for(int i = 0; i < m_double_window_size; i++)
      out << getInstructionByIndex(i).toString() << std::endl;
   return String(out.str().c_str());
}

String WindowStopDispatchReasonStringHelper(WindowStopDispatchReason r)
{
   switch(r)
   {
   case WIN_STOP_DISPATCH_NO_REASON:
      return String("NoReason");
   case WIN_STOP_DISPATCH_FP_ADDSUB:
      return String("FpAddSub");
   case WIN_STOP_DISPATCH_FP_MULDIV:
      return String("FpMulDiv");
   case WIN_STOP_DISPATCH_LOAD:
      return String("Load");
   case WIN_STOP_DISPATCH_STORE:
      return String("Store");
   case WIN_STOP_DISPATCH_GENERIC:
      return String("Generic");
   case WIN_STOP_DISPATCH_BRANCH:
      return String("Branch");
   default:
      return String("UnknownWindowStopDispatchReason");
   }
}

String WindowStopDispatchReasonString(WindowStopDispatchReason r)
{
   if (r == WIN_STOP_DISPATCH_NO_REASON)
   {
      return WindowStopDispatchReasonStringHelper(r);
   }
   String s;
   for (int i = 0 ; (0x1 << i) < WIN_STOP_DISPATCH_SIZE ; i++ )
   {
      if ( (r >> i) & 0x1 )
      {
         if (s != "")
         {
            s += "+";
         }
         s += WindowStopDispatchReasonStringHelper((WindowStopDispatchReason)(0x1 << i));
      }
   }
   return s;
}

CpContrType getCpContrType(const MicroOp& uop)
{
   switch(uop.getUopType()) {
      case MicroOp::UOP_TYPE_FP_ADDSUB:
         return CPCONTR_TYPE_FP_ADDSUB;
      case MicroOp::UOP_TYPE_FP_MULDIV:
         return CPCONTR_TYPE_FP_MULDIV;
      case MicroOp::UOP_TYPE_LOAD:
         switch(uop.getDCacheHitWhere()) {
            case HitWhere::L1_OWN:
               return CPCONTR_TYPE_LOAD_L1;
            case HitWhere::L2_OWN:
            case HitWhere::L1_SIBLING:
            case HitWhere::L2_SIBLING:
               return CPCONTR_TYPE_LOAD_L2;
            case HitWhere::L3_OWN:
            case HitWhere::L3_SIBLING:
               return CPCONTR_TYPE_LOAD_L3;
            default:
               return CPCONTR_TYPE_LOAD_LX;
         }
      case MicroOp::UOP_TYPE_STORE:
         return CPCONTR_TYPE_STORE;
      case MicroOp::UOP_TYPE_GENERIC:
         return CPCONTR_TYPE_GENERIC;
      case MicroOp::UOP_TYPE_BRANCH:
         return CPCONTR_TYPE_BRANCH;
      default:
         LOG_ASSERT_ERROR(false, "Unknown uop_type %d", uop.getUopType());
   }
   assert(false);
}

String CpContrTypeString(CpContrType type)
{
   switch(type) {
      case CPCONTR_TYPE_FP_ADDSUB:
         return "fp_addsub";
      case CPCONTR_TYPE_FP_MULDIV:
         return "fp_muldiv";
      case CPCONTR_TYPE_LOAD_L1:
         return "load_l1";
      case CPCONTR_TYPE_LOAD_L2:
         return "load_l2";
      case CPCONTR_TYPE_LOAD_L3:
         return "load_l3";
      case CPCONTR_TYPE_LOAD_LX:
         return "load_other";
      case CPCONTR_TYPE_STORE:
         return "store";
      case CPCONTR_TYPE_GENERIC:
         return "generic";
      case CPCONTR_TYPE_BRANCH:
         return "branch";
      default:
         LOG_ASSERT_ERROR(false, "Unknown CpContrType %u", type);
         return "unknown";
   }
}
