#include <string.h>

#include "transport.h"
#include "core.h"
#include "network.h"
#include "memory_manager_base.h"
#include "simulator.h"
#include "core_manager.h"
#include "log.h"
#include "subsecond_time.h"
#include "performance_model.h"
#include "instruction.h"

// FIXME: Rework netCreateBuf and netExPacket. We don't need to
// duplicate the sender/receiver info the packet. This should be known
// by the transport layer and given to us. We also should be more
// intelligent about the time stamps, right now the method is very
// ugly.

Network::Network(Core *core)
      : _core(core)
{
   LOG_ASSERT_ERROR(sizeof(g_type_to_static_network_map) / sizeof(EStaticNetwork) == NUM_PACKET_TYPES,
                    "Static network type map has incorrect number of entries.");

   _numMod = Config::getSingleton()->getTotalCores();
   _tid = _core->getId();

   _transport = Transport::getSingleton()->createNode(_core->getId());

   _callbacks = new NetworkCallback [NUM_PACKET_TYPES];
   _callbackObjs = new void* [NUM_PACKET_TYPES];
   for (SInt32 i = 0; i < NUM_PACKET_TYPES; i++)
      _callbacks[i] = NULL;

   UInt32 modelTypes[NUM_STATIC_NETWORKS];
   Config::getSingleton()->getNetworkModels(modelTypes);

   for (SInt32 i = 0; i < NUM_STATIC_NETWORKS; i++)
      _models[i] = NetworkModel::createModel(this, modelTypes[i], (EStaticNetwork)i);

   LOG_PRINT("Initialized.");
}

Network::~Network()
{
   for (SInt32 i = 0; i < NUM_STATIC_NETWORKS; i++)
      delete _models[i];

   delete [] _callbackObjs;
   delete [] _callbacks;

   delete _transport;

   LOG_PRINT("Destroyed.");
}

void Network::registerCallback(PacketType type, NetworkCallback callback, void *obj)
{
   assert((UInt32)type < NUM_PACKET_TYPES);
   _callbacks[type] = callback;
   _callbackObjs[type] = obj;
}

void Network::unregisterCallback(PacketType type)
{
   assert((UInt32)type < NUM_PACKET_TYPES);
   _callbacks[type] = NULL;
}

// Polling function that performs background activities, such as
// pulling from the physical transport layer and routing packets to
// the appropriate queues.

void Network::netPullFromTransport()
{
   do
   {
      LOG_PRINT("Entering netPullFromTransport");

      NetPacket packet(_transport->recv());

      LOG_PRINT("Pull packet : type %i, from %i, time %s", (SInt32)packet.type, packet.sender, itostr(packet.time).c_str());
      assert(0 <= packet.sender && packet.sender < _numMod);
      LOG_ASSERT_ERROR(0 <= packet.type && packet.type < NUM_PACKET_TYPES, "Packet type: %d not between 0 and %d", packet.type, NUM_PACKET_TYPES);

      // was this packet sent to us, or should it just be forwarded?
      if (packet.receiver != _core->getId())
      {
         // Disable this feature now. None of the network models use it
         LOG_PRINT("Forwarding packet : type %i, from %i, to %i, core_id %i, time %s.",
               (SInt32)packet.type, packet.sender, packet.receiver, _core->getId(), itostr(packet.time).c_str());
         forwardPacket(packet);

         // if this isn't a broadcast message, then we shouldn't process it further
         if (packet.receiver != NetPacket::BROADCAST)
         {
            if (packet.length > 0)
               delete [] (Byte*) packet.data;
            continue;
         }
      }

      // I have received the packet
      NetworkModel *model = _models[g_type_to_static_network_map[packet.type]];
      model->processReceivedPacket(packet);

      // asynchronous I/O support
      NetworkCallback callback = _callbacks[packet.type];

      if (callback != NULL)
      {
         LOG_PRINT("Executing callback on packet : type %i, from %i, to %i, core_id %i, time %s",
               (SInt32)packet.type, packet.sender, packet.receiver, _core->getId(), itostr(packet.time).c_str());
         assert(0 <= packet.sender && packet.sender < _numMod);
         assert(0 <= packet.type && packet.type < NUM_PACKET_TYPES);

         callback(_callbackObjs[packet.type], packet);

         if (packet.length > 0)
            delete [] (Byte*) packet.data;
      }

      // synchronous I/O support
      else
      {
         LOG_PRINT("Enqueuing packet : type %i, from %i, to %i, core_id %i, time %s.",
               (SInt32)packet.type, packet.sender, packet.receiver, _core->getId(), itostr(packet.time).c_str());
         _netQueueLock.acquire();
         _netQueue.push_back(packet);
         _netQueueLock.release();
         _netQueueCond.broadcast();
      }
   }
   while (_transport->query());
}

// FIXME: Can forwardPacket be subsumed by netSend?

void Network::forwardPacket(NetPacket& packet)
{
   netSend(packet);
}

NetworkModel* Network::getNetworkModelFromPacketType(PacketType packet_type)
{
   return _models[g_type_to_static_network_map[packet_type]];
}

SInt32 Network::netSend(NetPacket& packet)
{
   assert(packet.type >= 0 && packet.type < NUM_PACKET_TYPES);

   NetworkModel *model = _models[g_type_to_static_network_map[packet.type]];

   model->countPacket(packet);

   std::vector<NetworkModel::Hop> hopVec;
   model->routePacket(packet, hopVec);

   Byte *buffer = packet.makeBuffer();
   SubsecondTime start_time = packet.time;

   for (UInt32 i = 0; i < hopVec.size(); i++)
   {
      LOG_PRINT("Send packet : type %i, from %i, to %i, next_hop %i, core_id %i, time %s",
            (SInt32) packet.type, packet.sender, hopVec[i].final_dest, hopVec[i].next_dest, _core->getId(), itostr(hopVec[i].time).c_str());
      // LOG_ASSERT_ERROR(hopVec[i].time >= packet.time, "hopVec[%d].time(%llu) < packet.time(%llu)", i, hopVec[i].time, packet.time);

      // Do a shortcut here
      if (hopVec[i].final_dest != NetPacket::BROADCAST)
      {
         // 1) Process Count = 1
         // 2) The broadcast tree network model is not used
         while (hopVec[i].next_dest != hopVec[i].final_dest)
         {
            packet.time = hopVec[i].time;
            packet.receiver = hopVec[i].final_dest;

            Core* remote_core = Sim()->getCoreManager()->getCoreFromID(hopVec[i].next_dest);
            NetworkModel* remote_network_model = remote_core->getNetwork()->getNetworkModelFromPacketType(packet.type);

            std::vector<NetworkModel::Hop> localHopVec;
            remote_network_model->routePacket(packet, localHopVec);
            assert(localHopVec.size() == 1);

            hopVec[i] = localHopVec[0];
         }
      }

      NetPacket* buff_pkt = (NetPacket*) buffer;

      if (_core->getId() == buff_pkt->sender)
         buff_pkt->start_time = start_time;

      buff_pkt->time = hopVec[i].time;
      buff_pkt->receiver = hopVec[i].final_dest;

      _transport->send(hopVec[i].next_dest, buffer, packet.bufferSize());

      LOG_PRINT("Sent packet");
   }

   delete [] buffer;

   return packet.length;
}

// Stupid helper class to eliminate special cases for empty
// sender/type vectors in a NetMatch
class NetRecvIterator
{
   public:
      NetRecvIterator(UInt32 i)
            : _mode(INT)
            , _max(i)
            , _i(0)
      {
      }
      NetRecvIterator(const std::vector<SInt32> &v)
            : _mode(SENDER_VECTOR)
            , _senders(&v)
            , _i(0)
      {
      }
      NetRecvIterator(const std::vector<PacketType> &v)
            : _mode(TYPE_VECTOR)
            , _types(&v)
            , _i(0)
      {
      }

      inline UInt32 get()
      {
         switch (_mode)
         {
         case INT:
            return _i;
         case SENDER_VECTOR:
            return (UInt32)_senders->at(_i);
         case TYPE_VECTOR:
            return (UInt32)_types->at(_i);
         default:
            assert(false);
            return (UInt32)-1;
         };
      }

      inline Boolean done()
      {
         switch (_mode)
         {
         case INT:
            return _i >= _max;
         case SENDER_VECTOR:
            return _i >= _senders->size();
         case TYPE_VECTOR:
            return _i >= _types->size();
         default:
            assert(false);
            return true;
         };
      }

      inline void next()
      {
         ++_i;
      }

      inline void reset()
      {
         _i = 0;
      }

   private:
      enum
      {
         INT, SENDER_VECTOR, TYPE_VECTOR
      } _mode;

      union
      {
         UInt32 _max;
         const std::vector<SInt32> *_senders;
         const std::vector<PacketType> *_types;
      };

      UInt32 _i;
};

NetPacket Network::netRecv(const NetMatch &match, UInt64 timeout_ns)
{
   LOG_PRINT("Entering netRecv.");

   // Track via iterator to minimize copying
   NetQueue::iterator itr;
   Boolean found = false, retry = true;

   NetRecvIterator sender = match.senders.empty()
                            ? NetRecvIterator(_numMod)
                            : NetRecvIterator(match.senders);

   NetRecvIterator type = match.types.empty()
                          ? NetRecvIterator((UInt32)NUM_PACKET_TYPES)
                          : NetRecvIterator(match.types);

   LOG_ASSERT_ERROR(_core && _core->getPerformanceModel(),
                    "Core and/or performance model not initialized.");
   SubsecondTime start_time = _core->getPerformanceModel()->getElapsedTime();

   _netQueueLock.acquire();

   while (!found)
   {
      itr = _netQueue.end();

      // check every entry in the queue
      for (NetQueue::iterator i = _netQueue.begin();
            i != _netQueue.end();
            i++)
      {
         // only find packets that match
         for (sender.reset(); !sender.done(); sender.next())
         {
            if (i->sender != (SInt32)sender.get())
               continue;

            for (type.reset(); !type.done(); type.next())
            {
               if (i->type != (PacketType)type.get())
                  continue;

               found = true;

               // find the earliest packet
               if (itr == _netQueue.end() ||
                   itr->time > i->time)
               {
                  itr = i;
               }
            }
         }
      }

      if (!found)
      {
         if (retry)
         {
            // go to sleep until a packet arrives if none have been found
            _netQueueCond.wait(_netQueueLock, timeout_ns);

            // After waking from either timeout or cond.signal, retry once but then no more
            if (timeout_ns)
               retry = false;
         } else {
            // non-blocking: return a special packet with length == -1 to denote no match
            _netQueueLock.release();
            NetPacket packet;
            packet.length = UINT32_MAX;
            return packet;
         }
      }
   }

   assert(found == true && itr != _netQueue.end());
   assert(0 <= itr->sender && itr->sender < _numMod);
   assert(0 <= itr->type && itr->type < NUM_PACKET_TYPES);
   assert((itr->receiver == _core->getId()) || (itr->receiver == NetPacket::BROADCAST));

   // Copy result
   NetPacket packet = *itr;
   _netQueue.erase(itr);
   _netQueueLock.release();

   LOG_PRINT("packet.time(%s), start_time(%s)", itostr(packet.time).c_str(), itostr(start_time).c_str());

   if (packet.time > start_time)
   {
      if (_core->getId() < (core_id_t)Sim()->getConfig()->getApplicationCores()) {
         if (packet.time - start_time < 100 * SubsecondTime::NS()) {
            ; // Allow small timing differences, usually before performance models are enabled
         } else {
            // We really should't do it this way. You're supposed to include the time as a data item in the packet,
            // and have the application layer increment time (queueing a valid *Instruction which will attribute
            // the time increment to the correct CPI component)
            LOG_ASSERT_ERROR(false, "RecvInstruction(%s) being queued", itostr(packet.time - start_time).c_str());
         }
      } else {
         // Not that non-application models even care about this (their core performance models are never enabled).
         // Why even bother?
         LOG_PRINT("Queueing RecvInstruction(%s)", itostr(packet.time - start_time).c_str());
         PseudoInstruction *i = new RecvInstruction(packet.time - start_time);
         _core->getPerformanceModel()->queuePseudoInstruction(i);
      }
   }

   LOG_PRINT("Exiting netRecv : type %i, from %i", (SInt32)packet.type, packet.sender);

   return packet;
}

// -- Wrappers

SInt32 Network::netSend(SInt32 dest, PacketType type, const void *buf, UInt32 len)
{
   NetPacket packet;
   assert(_core && _core->getPerformanceModel());
   packet.time = _core->getPerformanceModel()->getElapsedTime();
   packet.sender = _core->getId();
   packet.receiver = dest;
   packet.length = len;
   packet.type = type;
   packet.data = buf;

   return netSend(packet);
}

SInt32 Network::netBroadcast(PacketType type, const void *buf, UInt32 len)
{
   return netSend(NetPacket::BROADCAST, type, buf, len);
}

NetPacket Network::netRecv(SInt32 src, PacketType type, UInt64 timeout_ns)
{
   NetMatch match;
   match.senders.push_back(src);
   match.types.push_back(type);
   return netRecv(match, timeout_ns);
}

NetPacket Network::netRecvFrom(SInt32 src, UInt64 timeout_ns)
{
   NetMatch match;
   match.senders.push_back(src);
   return netRecv(match, timeout_ns);
}

NetPacket Network::netRecvType(PacketType type, UInt64 timeout_ns)
{
   NetMatch match;
   match.types.push_back(type);
   return netRecv(match, timeout_ns);
}

void Network::enableModels()
{
   for (int i = 0; i < NUM_STATIC_NETWORKS; i++)
   {
      _models[i]->enable();
   }
}

void Network::disableModels()
{
   for (int i = 0; i < NUM_STATIC_NETWORKS; i++)
   {
      _models[i]->disable();
   }
}

// Modeling
UInt32 Network::getModeledLength(const NetPacket& pkt)
{
   if (pkt.type == SHARED_MEM_1)
   {
      // packet_type + sender + receiver + length + shmem_msg.size()
      // 1 byte for packet_type
      // log2(core_id) for sender and receiver
      // 2 bytes for packet length
      UInt32 metadata_size = 1 + 2 * Config::getSingleton()->getCoreIDLength() + 2;
      UInt32 data_size = getCore()->getMemoryManager()->getModeledLength(pkt.data);
      return metadata_size + data_size;
   }
   else
   {
      return pkt.bufferSize();
   }
}

// -- NetPacket

NetPacket::NetPacket()
   : start_time(SubsecondTime::Zero())
   , time(SubsecondTime::Zero())
   , queue_delay(SubsecondTime::Zero())
   , type(INVALID_PACKET_TYPE)
   , sender(INVALID_CORE_ID)
   , receiver(INVALID_CORE_ID)
   , length(0)
   , data(0)
{
}

NetPacket::NetPacket(SubsecondTime t, PacketType ty, SInt32 s,
                     SInt32 r, UInt32 l, const void *d)
   : start_time(SubsecondTime::Zero())
   , time(t)
   , queue_delay(SubsecondTime::Zero())
   , type(ty)
   , sender(s)
   , receiver(r)
   , length(l)
   , data(d)
{
}


NetPacket::NetPacket(Byte *buffer)
{
   memcpy(this, buffer, sizeof(*this));

   // LOG_ASSERT_ERROR(length > 0, "type(%u), sender(%i), receiver(%i), length(%u)", type, sender, receiver, length);
   if (length > 0)
   {
      Byte* data_buffer = new Byte[length];
      memcpy(data_buffer, buffer + sizeof(*this), length);
      data = data_buffer;
   }

   delete [] buffer;
}

// This implementation is slightly wasteful because there is no need
// to copy the const void* value in the NetPacket when length == 0,
// but I don't see this as a major issue.
UInt32 NetPacket::bufferSize() const
{
   return (sizeof(*this) + length);
}

Byte* NetPacket::makeBuffer() const
{
   UInt32 size = bufferSize();
   assert(size >= sizeof(NetPacket));

   Byte *buffer = new Byte[size];

   memcpy(buffer, this, sizeof(*this));
   memcpy(buffer + sizeof(*this), data, length);

   return buffer;
}
