#ifndef OPERAND_H
#define OPERAND_H

#include "fixed_types.h"

#include <sstream>
#include <iostream>
#include <vector>

class Operand
{
public:
   enum Type
   {
      REG,
      MEMORY,
      IMMEDIATE
   };

   enum Direction
   {
      READ,
      WRITE
   };

   typedef UInt64 Value;

   Operand(const Operand &src)
      : m_type(src.m_type), m_value(src.m_value), m_direction(src.m_direction), m_value_name(src.m_value_name), m_mem_operand(src.m_mem_operand) {}

   Operand(Type type, Value value = 0, Direction direction = READ, const String& value_name = String(), bool mem_operand = false)
      : m_type(type), m_value(value), m_direction(direction), m_value_name(value_name), m_mem_operand(mem_operand) {}

   String toString (void) const
   {
      std::ostringstream o;
      o << "Operand: ";
      if (m_type == REG)
         o << "REG";
      if (m_type == MEMORY)
         o << "MEMORY";
      if (m_type == IMMEDIATE)
         o << "IMMEDIATE";

      o << " ";

      if (m_direction == READ)
         o << "READ";
      if (m_direction == WRITE)
         o << "WRITE";

      o << " ";

      if (m_type == REG)
         o << m_value_name << "[Operand:" << m_mem_operand << "] (" << m_value << ")";
      else
         o << m_value;

      return String(o.str().c_str());
   }

   Type m_type;
   Value m_value;
   Direction m_direction;
   String m_value_name; // for m_type == REG
   bool m_mem_operand; // for m_type == REG
};

typedef std::vector<Operand> OperandList;

#endif /* OPERAND_H */
