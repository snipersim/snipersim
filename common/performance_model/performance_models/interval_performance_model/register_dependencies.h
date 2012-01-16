#ifndef __REGISTER_DEPENDENCIES_H
#define __REGISTER_DEPENDENCIES_H

#include "fixed_types.h"
#include "micro_op.h" // For TOTAL_NUM_REGISTERS

class RegisterDependencies {
private:
    // Array containing the sequence number of the producers for each of the registers.
  uint64_t producers[TOTAL_NUM_REGISTERS];
public:
  RegisterDependencies();

  void setDependencies(MicroOp& microOp, uint64_t lowestValidSequenceNumber);

  void clear();
};

#endif /* __REGISTER_DEPENDENCIES_H */
