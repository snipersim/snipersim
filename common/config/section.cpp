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
    }

    //create a new section and add it to the subsection map
    Section & Section::addSubsection(const String & name_)
    {
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        m_subSections.insert(std::make_pair(iname, new Section(*this, name_, m_case_sensitive)));
        return *(m_subSections[iname].get());
    }

    //add a new key/value pair to this section
    const Key & Section::addKey(const String & name_, const String & value)
    {
        //Make the key all lower-case if not case sensitive
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        //Remove existing key if found
        KeyList::const_iterator found = m_keys.find(iname);
        if(found != m_keys.end())
            m_keys.erase(iname);

        m_keys.insert(std::make_pair(iname, new Key(this->getFullPath(),name_,value)));
        return *(m_keys[iname].get());
    }

    const Key & Section::addKey(const String & name_, const SInt64 value)
    {
        //Make the key all lower-case if not case sensitive
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        //Remove existing key if found
        KeyList::const_iterator found = m_keys.find(iname);
        if(found != m_keys.end())
            m_keys.erase(iname);

        m_keys.insert(std::make_pair(iname, new Key(this->getFullPath(),name_,value)));
        return *(m_keys[iname].get());
    }

    const Key & Section::addKey(const String & name_, const double value)
    {
        //Make the key all lower-case if not case sensitive
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);

        //Remove existing key if found
        KeyList::const_iterator found = m_keys.find(iname);
        if(found != m_keys.end())
            m_keys.erase(iname);

        m_keys.insert(std::make_pair(iname, new Key(this->getFullPath(),name_,value)));
        return *(m_keys[iname].get());
    }

    //get a subkey of the given name
    const Key & Section::getKey(const String & name_)
    {
        String iname(name_);
        if(!m_case_sensitive)
            boost::to_lower(iname);
        return *(m_keys[iname].get());
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
        return *(m_subSections[iname].get());
    }

    bool Section::hasSection(const String & name) const
    {
        String iname(name);
        if(!m_case_sensitive)
            boost::to_lower(iname);
        SectionList::const_iterator found = m_subSections.find(iname);
        return (found != m_subSections.end());
    }

    bool Section::hasKey(const String & name) const
    {
        String iname(name);
        if(!m_case_sensitive)
            boost::to_lower(iname);
        KeyList::const_iterator found = m_keys.find(iname);
        return (found != m_keys.end());
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


