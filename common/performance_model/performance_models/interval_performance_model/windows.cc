/*
 * This file is covered under the Interval Academic License, see LICENCE.interval
 */

#include "windows.h"
#include "log.h"
#include "instruction.h"
#include "core_model.h"
#include "interval_contention.h"

#include <algorithm>

#include <sstream>
#include <iostream>
#include <iomanip>
#include <assert.h>

void Windows::WindowEntry::initialize(DynamicMicroOp* micro_op)
{
   if (this->uop)
   {
      // Delete our old DynamicMicroOp before we overwrite it
      delete this->uop;
   }

   this->uop = micro_op;
   this->execTime = 0;
   this->dispatchTime = 0;
   this->fetchTime = 0;
   this->cpContr = 0;
   this->cphead = 0;
   this->cptail = 0;
   this->maxProducer = 0;
   this->overlapFlags = 0;
   this->dependent = NO_DEP;
}

Windows::Windows(int window_size, bool doFunctionalUnitContention, Core *core, const CoreModel *core_model)
   : m_core_model(core_model)
   , m_interval_contention(core_model->createIntervalContentionModel(core))
   , m_double_window(new WindowEntry[2*window_size])
   , m_exec_time_map(new uint32_t[2*window_size])
   , m_do_functional_unit_contention(doFunctionalUnitContention)
   , m_register_dependencies(new RegisterDependencies())
   , m_memory_dependencies(new MemoryDependencies())
{
   m_window_size = window_size;
   m_double_window_size = 2*window_size;

   for(int i = 0; i < m_double_window_size; i++)
   {
      m_double_window[i].uop = NULL;
      m_double_window[i].setWindowIndex(i);
   }

   clear();
   clearFunctionalUnitStats();
}

Windows::~Windows()
{
   for (int i = 0; i < m_double_window_size; i++)
      if (m_double_window[i].uop)
         delete m_double_window[i].uop;
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
   for(unsigned int i = 0; i < (unsigned int)CPCONTR_TYPE_SIZE; ++i)
   {
      m_cpcontr_bytype[i] = 0;
   }
   m_cpcontr_total = 0;
   m_interval_contention->clearFunctionalUnitStats();
}

void Windows::addFunctionalUnitStats(const WindowEntry &uop)
{
   m_cpcontr_bytype[getCpContrType(uop)] += uop.getCpContr();
   m_cpcontr_total += uop.getCpContr();
   m_interval_contention->addFunctionalUnitStats(uop.getDynMicroOp());
}

void Windows::removeFunctionalUnitStats(const WindowEntry &uop)
{
   m_cpcontr_bytype[getCpContrType(uop)] -= uop.getCpContr();
   m_cpcontr_total -= uop.getCpContr();
   m_interval_contention->removeFunctionalUnitStats(uop.getDynMicroOp());
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
void Windows::add(DynamicMicroOp* micro_op)
{
   LOG_ASSERT_ERROR(!wIsFull(), "Window is full");

   WindowEntry& entry = getInstructionByIndex(m_window_tail);
   m_window_tail = incrementIndex(m_window_tail);
   m_window_length++;
   micro_op->setSequenceNumber(m_next_sequence_number);
   m_next_sequence_number++;

   entry.initialize(micro_op);

   uint64_t lowestValidSequenceNumber = getInstructionByIndex(m_old_window_head).getSequenceNumber();
   m_register_dependencies->setDependencies(*micro_op, lowestValidSequenceNumber);
   m_memory_dependencies->setDependencies(*micro_op, lowestValidSequenceNumber);
}

Windows::WindowEntry& Windows::getInstructionByIndex(int index) const
{
   LOG_ASSERT_ERROR(index >= 0 && index < m_double_window_size, "Index is out of bounds");
   return m_double_window[index];
}

Windows::WindowEntry& Windows::getInstruction(uint64_t sequenceNumber) const
{
   Windows::WindowEntry& windowHead = getInstructionByIndex(m_window_head__old_window_tail);
   int distance = windowHead.getSequenceNumber() - sequenceNumber;
   int index = windowIndex(windowHead.getWindowIndex() - distance);

   Windows::WindowEntry& ret = getInstructionByIndex(index);
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

Windows::WindowEntry& Windows::getLastAdded() const
{
   return getInstructionByIndex(decrementIndex(m_window_tail));
}

Windows::WindowEntry& Windows::getInstructionToDispatch() const
{
   return getInstructionByIndex(m_window_head__old_window_tail);
}

Windows::WindowEntry& Windows::getOldestInstruction() const
{
   return getInstructionByIndex(m_old_window_head);
}

uint64_t Windows::getEffectiveCriticalPathLength(uint64_t critical_path_length, bool update_reason)
{
   if (!m_do_functional_unit_contention)
   {
      return critical_path_length;
   }
   else
   {
      return m_interval_contention->getEffectiveCriticalPathLength(critical_path_length, update_reason);
   }
}

void Windows::dispatchInstruction()
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

uint64_t Windows::longLatencyOperationLatency(WindowEntry& uop)
{
   if (uop.getExecTime() < m_critical_path_tail)
   {
      // No critical path extension
      return 0;
   }
   else if (uop.getExecTime() - m_critical_path_tail < m_core_model->getLongLatencyCutoff())
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

uint64_t Windows::updateCriticalPathTail(WindowEntry& uop)
{
   uint64_t execTime = uop.getExecTime();
   if (execTime > m_critical_path_tail) {
      if (execTime - m_critical_path_tail > m_core_model->getLongLatencyCutoff()) {
         // Extending the critical path by huge amounts isn't healthy (see Redmine #113).
         // Instead, the caller should add the long latency directly (and attribute it) and call clearOldWindow()
         LOG_PRINT_WARNING_ONCE("Warning: Updating the critical path by %lu > %u cycles", execTime - m_critical_path_tail, m_core_model->getLongLatencyCutoff());
      }
      uop.setCpContr(execTime - m_critical_path_tail);
      m_critical_path_tail = execTime;
   }
   return m_critical_path_tail;
}

int Windows::getMinimalFlushLatency(int width) const
{
   return (m_old_window_length+(width-1))/width;
}

uint64_t Windows::getCpContrFraction(CpContrType type, uint64_t effective_cp_length) const
{
   assert(m_cpcontr_bytype[type] <= m_cpcontr_total);
   if (m_cpcontr_total > 0)
      return 1000000 * m_cpcontr_bytype[type] / effective_cp_length;
   else
      return 0;
}

Windows::Iterator::Iterator(const Windows* const windows, int start, int stop): index(start), stop(stop), windows(windows) { }

bool Windows::Iterator::hasNext()
{
   return index != stop;
}

Windows::WindowEntry& Windows::Iterator::next()
{
   Windows::WindowEntry& ret = windows->getInstructionByIndex(index);
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
   Windows::WindowEntry& micro_op = getInstructionToDispatch();
   uint32_t br_resolution_latency = 0;

   // The m_exec_time_map is empty when the algorithm starts !

   // Mark direct producers of this instruction
   for(uint32_t i = 0; i < micro_op.getDynMicroOp()->getDependenciesLength(); i++)
   {
      if (oldWindowContains(micro_op.getDynMicroOp()->getDependency(i)))
      {
         Windows::WindowEntry& producer = getInstruction(micro_op.getDynMicroOp()->getDependency(i));
         m_exec_time_map[producer.getWindowIndex()] = producer.getDynMicroOp()->getExecLatency();
      }
   }

   // Find/mark producers of producers
   for (int i = windowIndex(m_window_head__old_window_tail - 1), j = 0; j < m_old_window_length; i = windowIndex(i - 1), j++)
   {
      if (m_exec_time_map[i])
      {
         // There is a path to the committed branch: check the dependencies
         Windows::WindowEntry& op = getInstructionByIndex(i);
         for (uint32_t k = 0; k < op.getDynMicroOp()->getDependenciesLength(); k++)
         {
            if (oldWindowContains(op.getDynMicroOp()->getDependency(k)))
            {
               Windows::WindowEntry& producer = getInstruction(op.getDynMicroOp()->getDependency(k));
               m_exec_time_map[producer.getWindowIndex()] = std::max((producer.getDynMicroOp()->getExecLatency() + m_exec_time_map[i]), m_exec_time_map[producer.getWindowIndex()]);
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
      out << getInstructionByIndex(i).getMicroOp()->toString() << std::endl;
   return String(out.str().c_str());
}

CpContrType getCpContrType(const Windows::WindowEntry& entry)
{
   const MicroOp &uop = *entry.getMicroOp();
   switch(uop.getSubtype()) {
      case MicroOp::UOP_SUBTYPE_FP_ADDSUB:
         return CPCONTR_TYPE_FP_ADDSUB;
      case MicroOp::UOP_SUBTYPE_FP_MULDIV:
         return CPCONTR_TYPE_FP_MULDIV;
      case MicroOp::UOP_SUBTYPE_LOAD:
         switch(entry.getDynMicroOp()->getDCacheHitWhere()) {
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
      case MicroOp::UOP_SUBTYPE_STORE:
         return CPCONTR_TYPE_STORE;
      case MicroOp::UOP_SUBTYPE_GENERIC:
         return CPCONTR_TYPE_GENERIC;
      case MicroOp::UOP_SUBTYPE_BRANCH:
         return CPCONTR_TYPE_BRANCH;
      default:
         LOG_PRINT_ERROR("Unknown uop_subtype %d", uop.getSubtype());
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
         LOG_PRINT_ERROR("Unknown CpContrType %u", type);
         return "unknown";
   }
}
