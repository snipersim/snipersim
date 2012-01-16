#ifndef BASIC_BLOCK_H
#define BASIC_BLOCK_H

#include "instruction.h"

#include <vector>

class BasicBlock : public std::vector<Instruction*>
{
public:
   BasicBlock(bool dynamic = false)
      : m_dynamic(dynamic)
      {}

   ~BasicBlock()
      {
         if (m_dynamic)
         {
            for (unsigned int i = 0; i < size(); i++)
            {
               delete (*this)[i];
            }
         }
      }

   bool isDynamic() { return m_dynamic; }

private:
   bool m_dynamic;
};

#endif
