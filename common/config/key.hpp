#ifndef BL_KEY_HPP
#define BL_KEY_HPP
/*!
 *\file
 *
 */
// Config Class
// Author: Charles Gruenwald III

#include "fixed_types.h"

namespace config
{
    //! Enumeration for the types a given key may be
    enum KeyType
    {
        TYPE_INT_VALID = 0x01,
        TYPE_FLOAT_VALID = 0x02,
        TYPE_STRING_VALID = 0x04,
        TYPE_BOOL_VALID = 0x08
    };


    /*! \brief Key: A configuration setting entry
     * This class is used to hold a given setting from a configuration.
     * It contains the actual data, as well as functions to get the type
     */
    class Key
    {
        public:
            //! \brief Constructor to create a key
            template <class V>
            Key(const String & parentPath, const String & name, const V & value);

            const String getString() const;
            //Note: The following may throw boost::bad_lexial_cast
            bool getBool() const;
            SInt64 getInt() const;
            double getFloat() const;

            void getValue(bool &bool_val) const;
            void getValue(SInt64 &int_val) const;
            void getValue(String &string_val) const;
            void getValue(double &double_val) const;

            bool getFloatValid(){ return (bool)(m_type & TYPE_FLOAT_VALID); }
            bool getIntValid(){ return (bool)(m_type & TYPE_INT_VALID); }
            bool getBoolValid(){ return (bool)(m_type & TYPE_BOOL_VALID); }
            bool getStringValid(){ return true; }

            const String getName() const { return m_name; }

        private:
            unsigned short DetermineType(String);
            void throwInvalid(String) const;
            String m_name;

            String m_value;
            double m_value_f;
            SInt64 m_value_i;
            bool m_value_b;

            String m_parentPath;
            const unsigned short m_type;
    };

}//end of namespace config

#endif //KEY_HPP
