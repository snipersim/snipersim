#ifndef __PACKET_TYPE_H__
#define __PACKET_TYPE_H__

enum PacketType
{
   INVALID_PACKET_TYPE,
   USER_1,
   USER_2,
   SHARED_MEM_1,
   SHARED_MEM_2,
   SIM_THREAD_TERMINATE_THREADS,
   CORE_THREAD_TERMINATE_THREADS,
   SYSTEM_INITIALIZATION_NOTIFY,
   SYSTEM_INITIALIZATION_ACK,
   SYSTEM_INITIALIZATION_FINI,
   NUM_PACKET_TYPES
};

// This defines the different static network types
enum EStaticNetwork
{
   STATIC_NETWORK_USER_1,
   STATIC_NETWORK_USER_2,
   STATIC_NETWORK_MEMORY_1,
   STATIC_NETWORK_MEMORY_2,
   STATIC_NETWORK_SYSTEM,
   NUM_STATIC_NETWORKS
};

extern const char* EStaticNetworkStrings[];

// Packets are routed to a static network based on their type. This
// gives the static network to use for a given packet type.
static EStaticNetwork g_type_to_static_network_map[] __attribute__((unused)) =
{
   STATIC_NETWORK_SYSTEM,        // INVALID_PACKET_TYPE
   STATIC_NETWORK_USER_1,        // USER_1
   STATIC_NETWORK_USER_2,        // USER_2
   STATIC_NETWORK_MEMORY_1,      // SM_1
   STATIC_NETWORK_MEMORY_2,      // SM_2
   STATIC_NETWORK_SYSTEM,        // ST_TERMINATE_THREADS
   STATIC_NETWORK_SYSTEM,        // CT_TERMINATE_THREADS
   STATIC_NETWORK_SYSTEM,        // SYSTEM_INITIALIZATION_NOTIFY
   STATIC_NETWORK_SYSTEM,        // SYSTEM_INITIALIZATION_ACK
   STATIC_NETWORK_SYSTEM,        // SYSTEM_INITIALIZATION_FINI
};

#endif
