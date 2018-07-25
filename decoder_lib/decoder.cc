#include "decoder.h"
#include "x86_decoder.h"
#if BUILD_RISCV
#include "riscv_decoder.h"
#endif

namespace dl 
{

// Decoder

Decoder::~Decoder() {}

dl_arch Decoder::get_arch()
{
  return m_arch;
}

dl_mode Decoder::get_mode()
{
  return m_mode;
}

dl_syntax Decoder::get_syntax()
{
  return m_syntax;
}

// DecodedInst

DecodedInst::~DecodedInst() {}

const bool & DecodedInst::get_already_decoded() const
{
  return m_already_decoded;
}

void DecodedInst::set_already_decoded(const bool & b)
{
  m_already_decoded = b;
}

size_t & DecodedInst::get_size()
{
  return m_size;
}

const uint8_t * & DecodedInst::get_code()
{
  return m_code;
}

uint64_t & DecodedInst::get_address()
{
  return m_address;
}

// DecoderFactory
  
Decoder *DecoderFactory::CreateDecoder(dl_arch arch, dl_mode mode, dl_syntax syntax)
{
  switch(arch)
  {
    case DL_ARCH_INTEL:
      return new X86Decoder(arch, mode, syntax);
    case DL_ARCH_RISCV:
#if BUILD_RISCV
      return new RISCVDecoder(arch, mode, syntax);
#else
      return NULL;
#endif
  }
  return NULL;
}

DecodedInst *DecoderFactory::CreateInstruction(Decoder * d, const uint8_t * code, size_t size, uint64_t addr)
{
  dl_arch arch = d->get_arch();
  
  switch(arch)
  {
    case DL_ARCH_INTEL:
      return new X86DecodedInst(d, code, size, addr);
    case DL_ARCH_RISCV:
#if BUILD_RISCV
      return new RISCVDecodedInst(d, code, size, addr);
#else
      return NULL;
#endif
  }
  return NULL;  
}

} // namespace dl;
