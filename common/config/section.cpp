/*!
 *\file
*/
// Config Class
// Author: Charles Gruenwald III
#include "section.hpp"
#include <boost/algorithm/string.hpp>
#include <iostream>

namespace config
{

    //! Section() Constructor for a root section
    Section::Section(const String & name, bool case_sensitive)
        :
            m_name(name),
            m_subSections(),
            m_keys(),
            m_parent(*this),
            m_isroot(true),
            m_case_sensitive(case_sensitive)
    {
    }

    //! Section() Constructor for a subsection
    Section::Section(Section const & parent, const String & name, bool case_sensitive)
        :
            m_name(name),
            m_subSections(),
            m_keys(),
            m_parent(parent),
            m_isroot(false),
            m_case_sensitive(case_sensitive)
    {
    }

    //! ~Section() Destructor
    Section::~Section()
    {
        m_subSections.clear();
        m_keys.clear();
        m_array_keys.clear();
    }

    //create a new section and add it to the subsection map
    Section & Section::addSubsection(const String & name_)
    {
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        m_subSections.insert(std::pair< String, Section* >(iname, new Section(*this, name_, m_case_sensitive)));
        return *(m_subSections[iname]);
    }

    template <class V>
    const Key & Section::addKeyInternal(const String & name_, const V & value, uint64_t index)
    {
        //Make the key all lower-case if not case sensitive
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        // Check for non-index version
        if (index == UINT64_MAX)
        {
            //Remove existing default key
            m_keys.erase(iname);

            //Remove overrides
            m_array_keys.erase(iname);

            m_keys.insert(std::pair< String, Key* >(iname, new Key(this->getFullPath(),name_,value)));
            return *(m_keys[iname]);
        }
        else
        {
            //Do not remove existing default key for override case

            UInt64 needed_size = index + 1;
            KeyArrayList::iterator found = m_array_keys.find(iname);
            if(found != m_array_keys.end())
            {
                std::vector<Key*> & arr = (*found).second;
                if (arr.size() < needed_size)
                {
                    arr.resize(needed_size);
                }
                else
                {
                    arr[index] = NULL;
                }
            }
            else
            {
                m_array_keys[iname] = std::vector<Key*>(needed_size);
            }

            m_array_keys[iname][index] = new Key(this->getFullPath(),name_,value);

            return *(m_array_keys[iname][index]);
        }
    }

    template const Key & Section::addKeyInternal(const String &, const String &, UInt64);
    template const Key & Section::addKeyInternal(const String &, const SInt64 &, UInt64);
    template const Key & Section::addKeyInternal(const String &, const double &, UInt64);

    //get a subkey of the given name
    // Not to be called unless the key exists (see hasKey())
    const Key & Section::getKey(const String & name_, uint64_t index)
    {
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        if (index == UINT64_MAX)
        {
            // Default to using non-index version
            return *(m_keys[iname]);
        }
        else
        {
            if ( (m_array_keys.find(iname) != m_array_keys.end()) &&
                 (m_array_keys[iname].size() >= (index+1))        &&
                 (m_array_keys[iname][index] != NULL)          )
            {
                // If we have the key as an override, use it
                return *(m_array_keys[iname][index]);
            }
            else
            {
                // Otherwise, return the value requested from the non-indexed version
                return *(m_keys[iname]);
            }
        }
    }

    //get a subsection of the given name
    const Section & Section::getSection(const String & name)
    {
        return getSection_unsafe(name);
    }

    //Unsafe version of the getSection function, this should only be used internally
    Section & Section::getSection_unsafe(const String & name)
    {
        String iname(name);
        if(!m_case_sensitive)
            boost::to_lower(iname);
        return *(m_subSections[iname]);
    }

    bool Section::hasSection(const String & name) const
    {
        String iname(name);
        if(!m_case_sensitive)
            boost::to_lower(iname);
        SectionList::const_iterator found = m_subSections.find(iname);
        return (found != m_subSections.end());
    }

    bool Section::hasKey(const String & name, UInt64 index) const
    {
        String iname(name);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        if (index == UINT64_MAX)
        {
            if (m_keys.count(iname))
               return true;
            else if (m_array_keys.count(iname))
               return true;
            else
               return false;
        }
        else
        {
            return ( ( (m_array_keys.find(iname) != m_array_keys.end()) &&
                       (m_array_keys.find(iname)->second.size() >= (index+1))        &&
                       (m_array_keys.find(iname)->second[index] != NULL)        )
                     ||
                       (m_keys.find(iname) != m_keys.end()) );
        }
    }

    const String Section::getFullPath() const
    {
        String path = "";
        if(isRoot())
            return path;

        if(getParent().isRoot())
            return getName();

        //create the path from the parent section and this section's name
        path = String(getParent().getFullPath()) + "/" + getName();
        return path;
    }

}//end of namespace config


