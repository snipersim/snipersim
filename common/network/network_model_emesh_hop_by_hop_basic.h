#ifndef __NETWORK_MODEL_EMESH_HOP_BY_HOP_BASIC_H__
#define __NETWORK_MODEL_EMESH_HOP_BY_HOP_BASIC_H__

#include "network_model_emesh_hop_by_hop_generic.h"

class NetworkModelEMeshHopByHopBasic : public NetworkModelEMeshHopByHopGeneric
{
   public:
      NetworkModelEMeshHopByHopBasic(Network* net);
      ~NetworkModelEMeshHopByHopBasic() {}
};

#endif /* __NETWORK_MODEL_EMESH_HOP_BY_HOP_BASIC_H__ */
