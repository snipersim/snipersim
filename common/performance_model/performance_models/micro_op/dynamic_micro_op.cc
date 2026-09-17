#include "dynamic_micro_op.h"
#include "log.h"
#include "core_model.h"
#include "instruction.h"
#include "micro_op.h"
#include "debug_config.h"
#include <cstdio>

DynamicMicroOp::DynamicMicroOp(const MicroOp *uop, const CoreModel *core_model, ComponentPeriod period)
   : m_uop(uop)
   , m_core_model(core_model)
   , m_instruction(nullptr)
   , m_period(period)
{
   // LOG_ASSERT_ERROR(period != SubsecondTime::Zero(), "MicroOp Period is == SubsecondTime::Zero()");

   this->squashed = false;

   this->branchTaken = false;
   this->branchMispredicted = false;

   this->intraInstructionDependencies = uop->intraInstructionDependencies;
   this->microOpTypeOffset = uop->microOpTypeOffset;
   this->squashedCount = 0;
   this->dependenciesLength = 0;

   /* A load or store uop that never receives a dynamic address (a predicated-off
      or unmapped access) must still carry a defined size (0: the static size). */
   this->address.set(0);
   this->execLatency = m_core_model->getInstructionLatency(uop);

   this->sequenceNumber = INVALID_SEQNR;

   this->dCacheHitWhere = HitWhere::UNKNOWN;
   this->iCacheHitWhere = HitWhere::L1I; // Default to an icache hit
   this->iCacheLatency = 0;

   this->m_forceLongLatencyLoad = false;

   for(uint32_t i = 0 ; i < MAXIMUM_NUMBER_OF_DEPENDENCIES; i++)
      this->dependencies[i] = -1;

   LOG_ASSERT_ERROR(m_uop != NULL, "uop is NULL");

   first = m_uop->isFirst();
   last = m_uop->isLast();

   // For dynamic instructions (ChampSim traces), add a reference so we can
   // safely clean up when all DynamicMicroOps from this instruction are freed.
   // Store direct pointer to Instruction for safe access in destructor
   // (m_uop may become invalid after another DynamicMicroOp frees the instruction).
   Instruction* instr = m_uop->getInstruction();
   if (instr && instr->isDynamic()) {
      m_instruction = instr;
      m_instruction->addRef();
      #if DEBUG_DYNAMIC_MICROOP >= DEBUG_DETAILED
      printf("[DYNAMIC_MICROOP] CTOR DynamicMicroOp*=%p -> Instruction*=%p addRef() refcount=%d\n",
             (void*)this, (void*)m_instruction, m_instruction->getRefCount());
      #endif
   }
}

DynamicMicroOp::~DynamicMicroOp()
{
   // For dynamic instructions (ChampSim traces), release our reference.
   // When the last DynamicMicroOp is destroyed, we clean up the Instruction
   // and all its MicroOps.
   // Use m_instruction directly (stored in constructor) rather than
   // m_uop->getInstruction() which may be invalid if another DynamicMicroOp
   // already freed the instruction and its MicroOps.
   if (m_instruction) {
      #if DEBUG_DYNAMIC_MICROOP >= DEBUG_BASIC
      int old_refcount = m_instruction->getRefCount();
      #endif
      if (m_instruction->release()) {
         // We were the last reference - clean up the instruction and its microops
         #if DEBUG_DYNAMIC_MICROOP >= DEBUG_BASIC
         printf("[DYNAMIC_MICROOP] DTOR DynamicMicroOp*=%p -> Instruction*=%p LAST REF (was %d) -> FREEING\n",
                (void*)this, (void*)m_instruction, old_refcount);
         #endif
         const std::vector<const MicroOp*>* uops = m_instruction->getMicroOps();
         if (uops) {
            for (const MicroOp* uop : *uops) {
               delete uop;
            }
            delete uops;
         }
         delete m_instruction;
      } else {
         #if DEBUG_DYNAMIC_MICROOP >= DEBUG_DETAILED
         printf("[DYNAMIC_MICROOP] DTOR DynamicMicroOp*=%p -> Instruction*=%p release() refcount=%d->%d\n",
                (void*)this, (void*)m_instruction, old_refcount, m_instruction->getRefCount());
         #endif
      }
   }
}

void DynamicMicroOp::squash(std::vector<DynamicMicroOp*>* array)
{
   squashed = true;

   if (array)
   {
      // Fix up isFirst/isLast after possibly squashing the first/last microop in a list
      for(int index = 0; index < (int)array->size(); ++index)
         if (!(*array)[index]->isSquashed())
         {
            (*array)[index]->setFirst(true);
            break;
         }
      for(int index = array->size() - 1; index >= 0; --index)
         if (!(*array)[index]->isSquashed())
         {
            (*array)[index]->setLast(true);
            break;
         }
   }
}

uint64_t DynamicMicroOp::getDependency(uint32_t index) const
{
   if (index < this->intraInstructionDependencies) {
      return this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies + index;
   } else {
      assert((index >= this->intraInstructionDependencies) && ((index - this->intraInstructionDependencies) < this->dependenciesLength));
      return this->dependencies[index - this->intraInstructionDependencies];
   }
}

void DynamicMicroOp::addDependency(uint64_t sequenceNumber)
{
   if (!Tools::contains(dependencies, dependenciesLength, sequenceNumber)) {
      assert(this->dependenciesLength < MAXIMUM_NUMBER_OF_DEPENDENCIES);
      dependencies[dependenciesLength] = sequenceNumber;
      dependenciesLength++;
   }
}

void DynamicMicroOp::removeDependency(uint64_t sequenceNumber)
{
   if (sequenceNumber >= this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies) {
      // Intra-instruction dependency
      while(intraInstructionDependencies && !(sequenceNumber == this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies)) {
         // Remove the first intra-instruction dependency, but since this is not the one to be removed, add it to the regular dependencies list
         dependencies[dependenciesLength] = this->sequenceNumber - this->microOpTypeOffset - this->intraInstructionDependencies;
         dependenciesLength++;
         LOG_ASSERT_ERROR(dependenciesLength < MAXIMUM_NUMBER_OF_DEPENDENCIES, "dependenciesLength(%u) > MAX(%u)", dependenciesLength, MAXIMUM_NUMBER_OF_DEPENDENCIES);
         intraInstructionDependencies--;
      }
      // Make sure the exit condition was that the dependency to be removed is now the first one, not that we have exhausted the list
      LOG_ASSERT_ERROR(intraInstructionDependencies > 0, "Something went wrong while removing an intra-instruction dependency");
      // Remove the first intra-instruction dependency by decrementing intraInstructionDependencies
      intraInstructionDependencies--;
   } else {
      // Inter-instruction dependency
      LOG_ASSERT_ERROR(dependenciesLength > 0, "Cannot remove dependency when there are none");
      if (dependencies[dependenciesLength-1] == sequenceNumber)
         ; // sequenceNumber to remove is already at the end, we can just decrement dependenciesLength
      else {
         // Move sequenceNumber to the end of the list
         uint64_t idx = Tools::index(dependencies, dependenciesLength, sequenceNumber);
         LOG_ASSERT_ERROR(idx != UINT64_MAX, "MicroOp dependency list does not contain %ld", sequenceNumber);
         Tools::swap(dependencies, idx, dependenciesLength-1);
      }
      dependenciesLength--;
   }
}

const Memory::Access& DynamicMicroOp::getLoadAccess() const
{
   assert(this->getMicroOp()->isLoad());
   return this->address;
}

const Memory::Access& DynamicMicroOp::getStoreAccess() const
{
   assert(this->getMicroOp()->isStore());
   return this->address;
}

bool DynamicMicroOp::isLongLatencyLoad() const
{
   LOG_ASSERT_ERROR(getMicroOp()->isLoad(), "Expected a load instruction.");

   uint32_t cutoff = m_core_model->getLongLatencyCutoff();

   // If we are enabled, indicate that this is a long latency load if the latency
   // is above a certain cutoff value
   // Also, honor the forceLLL request if indicated
   return (m_forceLongLatencyLoad || ((cutoff > 0) && (this->execLatency > cutoff)));
}
