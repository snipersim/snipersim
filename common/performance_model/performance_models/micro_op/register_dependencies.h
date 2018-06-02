#ifndef __REGISTER_DEPENDENCIES_H
#define __REGISTER_DEPENDENCIES_H

#include "fixed_types.h"
#include <decoder.h>

//extern "C" {
//#include <xed-reg-enum.h>
//}

class DynamicMicroOp;

class RegisterDependencies {
private:
    // Array containing the sequence number of the producers for each of the registers.
    // FIXME Depending on the architecture we may have too many elements
    // Not easy to get last element statically with the library
  uint64_t producers[280];  //XED_REG_LAST;
public:
  RegisterDependencies();

  void setDependencies(DynamicMicroOp& microOp, uint64_t lowestValidSequenceNumber);
  uint64_t peekProducer(dl::Decoder::decoder_reg reg, uint64_t lowestValidSequenceNumber);

  void clear();
};

#endif /* __REGISTER_DEPENDENCIES_H */
