#ifndef MAGIC_CLIENT_H
#define MAGIC_CLIENT_H

#include "fixed_types.h"

/* respond, when given, is cleared by a command whose caller must not be
   answered now (MagicServer::Magic) */
UInt64 handleMagicInstruction(thread_id_t thread_id, UInt64 cmd, UInt64 arg0, UInt64 arg1, bool *respond = nullptr);

void setInstrumentationMode(UInt64 opt);

#endif // MAGIC_CLIENT_H
