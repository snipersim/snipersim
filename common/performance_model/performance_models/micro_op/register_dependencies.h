#ifndef __REGISTER_DEPENDENCIES_H
#define __REGISTER_DEPENDENCIES_H

#include <decoder.h>
#include <vector>
#include <cstdint>

//extern "C" {
//#include <xed-reg-enum.h>
//}

class DynamicMicroOp;

class RegisterDependencies {
private:
  // Array containing the sequence number of the producers for each of the registers.
  std::vector<uint64_t> producers;
public:
  RegisterDependencies();

  void setDependencies(DynamicMicroOp& microOp, uint64_t lowestValidSequenceNumber);
  uint64_t peekProducer(dl::Decoder::decoder_reg reg, uint64_t lowestValidSequenceNumber);

  void clear();
};

#endif /* __REGISTER_DEPENDENCIES_H */
