#ifndef __NETWORK_MODEL_EMESH_HOP_BY_HOP_BROADCAST_TREE_H__
#define __NETWORK_MODEL_EMESH_HOP_BY_HOP_BROADCAST_TREE_H__

#include "network_model_emesh_hop_by_hop_generic.h"

class NetworkModelEMeshHopByHopBroadcastTree : public NetworkModelEMeshHopByHopGeneric
{
   public:
      NetworkModelEMeshHopByHopBroadcastTree(Network* net);
      ~NetworkModelEMeshHopByHopBroadcastTree() {}
};

#endif /* __NETWORK_MODEL_EMESH_HOP_BY_HOP_BROADCAST_TREE_H__ */
