#ifndef __CHEETAH_MANAGER_H
#define __CHEETAH_MANAGER_H

#include "fixed_types.h"
#include "core.h"

class CheetahModel;

class CheetahManager
{
   private:
      typedef enum {
         CHEETAH_LOCAL,
         CHEETAH_BY2,
         CHEETAH_BY4,
         CHEETAH_BY8,
         CHEETAH_GLOBAL,
         NUM_CHEETAH_TYPES
      } cheetah_types_t;
      static const char* cheetah_names[];

      class CheetahStats
      {
         private:
            const UInt32 m_min_bits;
            const UInt32 m_max_bits_local;
            const UInt32 m_max_bits_global;
            std::vector<std::vector<UInt64> > m_stats;

            static SInt64 hook_update(UInt64 user, UInt64 args)
            { ((CheetahStats*)user)->update(); return 0; }
            void update();

         public:
            CheetahStats(UInt32 min_bits, UInt32 max_bits_local, UInt32 max_bits_global);
      };
      static CheetahStats *s_cheetah_stats;
      static std::vector<std::vector<CheetahModel*> > s_cheetah_models;

      const UInt32 m_min_bits;
      const UInt32 m_max_bits_local;
      const UInt32 m_max_bits_global;
      CheetahModel *m_cheetah[NUM_CHEETAH_TYPES];

      static const UInt32 ADDRESS_BUFFER_SIZE = 256;
      IntPtr m_address_buffer[ADDRESS_BUFFER_SIZE];
      UInt32 m_address_buffer_size;

   public:
      CheetahManager(core_id_t core_id);
      ~CheetahManager();

      void access(Core::mem_op_t mem_op_type, IntPtr address);
};

#endif // __CHEETAH_MANAGER_H
