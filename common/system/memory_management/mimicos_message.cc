#include "mimicos_message.h"

// Static member definitions
std::unordered_map<std::string, uint64_t> MimicOSProtocol::s_encode_map = {
    {"page_fault",      MimicOSProtocol::PAGE_FAULT},
    {"syscall",         MimicOSProtocol::SYSCALL},
    {"new_thread",      MimicOSProtocol::NEW_THREAD},
    {"thread_exit",     MimicOSProtocol::THREAD_EXIT},
    {"quantum_expired", MimicOSProtocol::QUANTUM_EXPIRED},
    {"shutdown",        MimicOSProtocol::SHUTDOWN}
};

std::unordered_map<uint64_t, std::string> MimicOSProtocol::s_decode_map = {
    {MimicOSProtocol::PAGE_FAULT,      "page_fault"},
    {MimicOSProtocol::SYSCALL,         "syscall"},
    {MimicOSProtocol::NEW_THREAD,      "new_thread"},
    {MimicOSProtocol::THREAD_EXIT,     "thread_exit"},
    {MimicOSProtocol::QUANTUM_EXPIRED, "quantum_expired"},
    {MimicOSProtocol::SHUTDOWN,        "shutdown"}
};
