#ifndef __PACKET_TYPE_H__
#define __PACKET_TYPE_H__

enum PacketType
{
   INVALID_PACKET_TYPE,
   SHARED_MEM_1,
   SIM_THREAD_TERMINATE_THREADS,
   CORE_THREAD_TERMINATE_THREADS,
   NUM_PACKET_TYPES
};

// This defines the different static network types
enum EStaticNetwork
{
   STATIC_NETWORK_MEMORY_1,
   STATIC_NETWORK_SYSTEM,
   NUM_STATIC_NETWORKS
};

extern const char* EStaticNetworkStrings[];

// Packets are routed to a static network based on their type. This
// gives the static network to use for a given packet type.
static EStaticNetwork g_type_to_static_network_map[] __attribute__((unused)) =
{
   STATIC_NETWORK_SYSTEM,        // INVALID_PACKET_TYPE
   STATIC_NETWORK_MEMORY_1,      // SM_1
   STATIC_NETWORK_SYSTEM,        // ST_TERMINATE_THREADS
   STATIC_NETWORK_SYSTEM,        // CT_TERMINATE_THREADS

};

#endif
