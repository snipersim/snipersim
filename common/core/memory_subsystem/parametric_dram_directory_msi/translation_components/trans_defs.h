#include "payload_word.h"
#ifndef __TRANS_TYPES_H
#define __TRANS_TYPES_H

#include <cstdint>
    
typedef struct queue_entry_s
{
    SubsecondTime timestamp;
    int page_size;
    IntPtr ppn;
    IntPtr address;
    ParametricDramDirectoryMSI::PayloadWord payload_bits;  ///< Shadow PTE payload from leaf (for temporal prefetching)
} query_entry;


class Compare
{
public:
    bool operator()(query_entry a, query_entry b)
    {
        return a.timestamp > b.timestamp;
    }
};

#endif // __