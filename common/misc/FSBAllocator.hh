/*===========================================================================
  This library is released under the MIT license. See FSBAllocator.html
  for further information and documentation.

Copyright (c) 2008-2011 Juha Nieminen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
=============================================================================*/

#ifndef INCLUDE_FSBALLOCATOR_HH
#define INCLUDE_FSBALLOCATOR_HH

#include "log.h"

#include <new>
#include <vector>

template<unsigned ElemSize, unsigned MaxElem = 0>
class FSBAllocator_ElemAllocator
{
    typedef std::size_t Data_t;
    static const Data_t BlockElements = 512;

    static const Data_t DSize = sizeof(Data_t);
    static const Data_t ElemSizeInDSize = (ElemSize + (DSize-1)) / DSize;
    static const Data_t UnitSizeInDSize = ElemSizeInDSize + 1;
    static const Data_t BlockSize = BlockElements*UnitSizeInDSize;

    class MemBlock
    {
        Data_t* block;
        Data_t firstFreeUnitIndex, allocatedElementsAmount, endIndex;

     public:
        MemBlock():
            block(0),
            firstFreeUnitIndex(Data_t(-1)),
            allocatedElementsAmount(0)
        {}

        bool isFull() const
        {
            return allocatedElementsAmount == BlockElements;
        }

        void clear()
        {
            delete[] block;
            block = 0;
            firstFreeUnitIndex = Data_t(-1);
        }

        void* allocate(Data_t vectorIndex)
        {
            if(firstFreeUnitIndex == Data_t(-1))
            {
                if(!block)
                {
                    block = new Data_t[BlockSize];
                    if(!block) return 0;
                    endIndex = 0;
                }

                Data_t* retval = block + endIndex;
                endIndex += UnitSizeInDSize;
                retval[ElemSizeInDSize] = vectorIndex;
                ++allocatedElementsAmount;
                return retval;
            }
            else
            {
                Data_t* retval = block + firstFreeUnitIndex;
                firstFreeUnitIndex = *retval;
                ++allocatedElementsAmount;
                return retval;
            }
        }

        void deallocate(Data_t* ptr)
        {
            *ptr = firstFreeUnitIndex;
            firstFreeUnitIndex = ptr - block;

            if(--allocatedElementsAmount == 0)
                clear();
        }
    };

    struct BlocksVector
    {
        std::vector<MemBlock> data;

        BlocksVector() { data.reserve(1024); }

        ~BlocksVector()
        {
            for(std::size_t i = 0; i < data.size(); ++i)
                data[i].clear();
        }
    };

    BlocksVector blocksVector;
    std::vector<Data_t> blocksWithFree;

 public:
    void* allocate()
    {
        if(blocksWithFree.empty())
        {
            blocksWithFree.push_back(blocksVector.data.size());
            blocksVector.data.push_back(MemBlock());
            if (MaxElem)
               LOG_ASSERT_ERROR(blocksVector.data.size() * BlockElements <= MaxElem, "Maximum number of blocks exceeded for allocator of %d-sized objects", ElemSize);
        }

        const Data_t index = blocksWithFree.back();
        MemBlock& block = blocksVector.data[index];
        void* retval = block.allocate(index);

        if(block.isFull())
            blocksWithFree.pop_back();

        return retval;
    }

    void deallocate(void* ptr)
    {
        if(!ptr) return;

        Data_t* unitPtr = (Data_t*)ptr;
        const Data_t blockIndex = unitPtr[ElemSizeInDSize];
        MemBlock& block = blocksVector.data[blockIndex];

        if(block.isFull())
            blocksWithFree.push_back(blockIndex);
        block.deallocate(unitPtr);
    }
};


#endif
