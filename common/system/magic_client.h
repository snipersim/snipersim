#ifndef MAGIC_CLIENT_H
#define MAGIC_CLIENT_H

#include "fixed_types.h"

UInt64 handleMagicInstruction(thread_id_t thread_id, UInt64 cmd, UInt64 arg0, UInt64 arg1);

void setInstrumentationMode(UInt64 opt);

#endif // MAGIC_CLIENT_H
