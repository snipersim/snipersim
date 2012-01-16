#include <cassert>

#include "modulo_num.h"

ModuloNum::ModuloNum(UInt32 max_value, UInt32 value) :
   m_value(value),
   m_max_value(max_value)
{}


ModuloNum::~ModuloNum()
{}

ModuloNum
ModuloNum::operator+(const ModuloNum& num) const
{
   ModuloNum new_num(m_max_value);
   assert(m_max_value == num.getMaxValue());

   new_num.setValue((m_value + num.getValue()) % m_max_value);
   return new_num;
}

ModuloNum
ModuloNum::operator-(const ModuloNum& num) const
{
   ModuloNum new_num(m_max_value);
   assert(m_max_value == num.getMaxValue());

   if (m_value >= num.getValue())
      new_num.setValue(m_value - num.getValue());
   else
      new_num.setValue(m_max_value - num.getValue() + m_value);

   return new_num;
}

ModuloNum
ModuloNum::operator+(UInt32 value) const
{
   ModuloNum num(m_max_value);
   num.setValue(value % m_max_value);
   return (*this + num);
}

ModuloNum
ModuloNum::operator-(UInt32 value) const
{
   ModuloNum num(m_max_value);
   num.setValue(value % m_max_value);
   return (*this - num);
}

bool
ModuloNum::operator==(const ModuloNum& num) const
{
   return ((m_value == num.getValue()) && (m_max_value == num.getMaxValue()));
}

bool
ModuloNum::operator!=(const ModuloNum& num) const
{
   return (!(*this == num));
}
