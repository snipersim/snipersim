/*!
 *\file
*/
// Config Class
// Author: Charles Gruenwald III
#include <boost/algorithm/string.hpp>

#include "config.hpp"


namespace config
{

    bool Config::isLeaf(const String & path)
    {
        return !boost::find_first(path, "/");
    }

    //Configuration Management
    const Section & Config::getSection(const String & path)
    {
        return getSection_unsafe(path);
    }

    Section & Config::getSection_unsafe(const String & path)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(!m_root.hasSection(path))
                m_root.addSubsection(path);
            return m_root.getSection_unsafe(path);
        }

        //split up the path on "/", and loop through each entry of this
        //split to obtain the actual section
        PathElementList path_elements;
        Config::splitPathElements(path, path_elements);

        Section * current = &m_root;
        for(PathElementList::iterator path_element = path_elements.begin();
                path_element != path_elements.end(); path_element++)
        {
            //add the section if it doesn't already exist
            if(!current->hasSection(*path_element))
            {
                current->addSubsection(*path_element);
            }

            //Find the current element name as a sub section of the current section
            current = &(current->getSection_unsafe(*path_element));
        }
        return *current;
    }

    //Small wrapper which sets the m_path variable appropriatly and calls the virtual loadConfig()
    void Config::load(const String & path)
    {
        m_path = path;
        loadConfig();
    }

    void Config::clear()
    {
        m_root.clear();
    }

    const Key & Config::getKey(const String & path)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(!m_root.hasKey(path))
                throw KeyNotFound();
            else
                return m_root.getKey(path);
        }

        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section & section = getSection_unsafe(path_pair.first);

        //add the key if it doesn't already exist
        if(!section.hasKey(path_pair.second))
        {
            section.addKey(path_pair.second, "");
        }

        return section.getKey(path_pair.second);
    }


    const Key & Config::getKey(const String & path, SInt64 default_val)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(!m_root.hasKey(path))
                m_root.addKey(path, default_val);
            return m_root.getKey(path);
        }

        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section & section = getSection_unsafe(path_pair.first);

        //add the key if it doesn't already exist
        if(!section.hasKey(path_pair.second))
        {
            section.addKey(path_pair.second, default_val);
        }

        return section.getKey(path_pair.second);
    }

    const Key & Config::getKey(const String & path, double default_val)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(!m_root.hasKey(path))
                m_root.addKey(path, default_val);
            return m_root.getKey(path);
        }

        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section & section = getSection_unsafe(path_pair.first);

        //add the key if it doesn't already exist
        if(!section.hasKey(path_pair.second))
        {
            section.addKey(path_pair.second, default_val);
        }

        return section.getKey(path_pair.second);
    }

    const Key & Config::getKey(const String & path, const String &default_val)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(!m_root.hasKey(path))
                m_root.addKey(path, default_val);
            return m_root.getKey(path);
        }

        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section & section = getSection_unsafe(path_pair.first);

        //add the key if it doesn't already exist
        if(!section.hasKey(path_pair.second))
        {
            section.addKey(path_pair.second, default_val);
        }

        return section.getKey(path_pair.second);
    }

    const Section & Config::addSection(const String & path)
    {
        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section &parent = getSection_unsafe(path_pair.first);
        return parent.addSubsection(path_pair.second);
    }

    std::pair<String,String> Config::splitPath(const String & path)
    {
        //Throw away path_elements, just return base and key/section
        std::vector<String> path_elements;
        return Config::splitPathElements(path, path_elements);
    }

    std::pair<String,String> Config::splitPathElements(const String & path, PathElementList & path_elements)
    {
        //split up the path on "/", the last entry is the name of the key
        //Everything up to the last "/" is the 'base_path' (which will specify a section)

        boost::split(path_elements, path, boost::is_any_of("/"));

        //Grab the appropriate pieces from the split
        String key_name = path_elements[path_elements.size() - 1];
        String base_path = "";
        if(path.rfind("/") != String::npos)
            base_path = path.substr(0, path.rfind("/"));

        return PathPair(base_path,key_name);
    }

    const Key & Config::addKey(const String & path, const String & value)
    {
        //Handle the base case
        if(isLeaf(path))
            return m_root.addKey(path, value);

        PathPair path_pair = Config::splitPath(path);
        Section &parent = getSection_unsafe(path_pair.first);
        return parent.addKey(path_pair.second, value);
    }

    const Key & Config::addKey(const String & path, SInt64 value)
    {
        //Handle the base case
        if(isLeaf(path))
            return m_root.addKey(path, value);

        PathPair path_pair = Config::splitPath(path);
        Section &parent = getSection_unsafe(path_pair.first);
        return parent.addKey(path_pair.second, value);
    }

    const Key & Config::addKey(const String & path, double value)
    {
        //Handle the base case
        if(isLeaf(path))
            return m_root.addKey(path, value);

        PathPair path_pair = Config::splitPath(path);
        Section &parent = getSection_unsafe(path_pair.first);
        return parent.addKey(path_pair.second, value);
    }

    //Convert the in-memory representation into a string
    String Config::showTree(const Section & current, int depth)
    {
        String result = "";

        String ret = "";
        String tabs = "";
        for(int i=0;i<depth;i++)
            tabs = tabs.append("    ");

        //First loop through all the subsections
        SectionList const & subsections = current.getSubsections();
        for(SectionList::const_iterator i = subsections.begin(); i != subsections.end(); i++)
        {
            Section const & subsection = *(i->second.get());
            result += tabs + "Section: " + i->second->getName() + "\n";
            //recurse
            result += showTree(subsection, depth+1);
        }

        //Now add all the keys of this section
        KeyList const & keys = current.getKeys();
        for(KeyList::const_iterator i = keys.begin(); i != keys.end();i++)
        {
            result += tabs + "Key: " + i->second->getName() + " - " + i->second->getString() + "\n";
        }
        return result;
    }

    void Config::set(const String & path, const String & new_value)
    {
        addKey(path, new_value);
    }

    void Config::set(const String & path, SInt64 new_value)
    {
        addKey(path, new_value);
    }

    void Config::set(const String & path, double new_value)
    {
        addKey(path, new_value);
    }

    //Below are the getters which also handle default values
    bool Config::getBool(const String & path)
    {
        return getKey(path).getBool();
    }

    bool Config::getBool(const String & path, bool default_val)
    {
        String s;
        if (default_val)
           s = "true";
        else
           s = "false";
        return getKey(path, s).getBool();
    }

    bool Config::getBool(const String & path, const String & default_val)
    {
        return getKey(path, default_val).getBool();
    }

    bool Config::getBool(const String & path, const char * default_val)
    {
        return getKey(path, default_val).getBool();
    }

    SInt64 Config::getInt(const String & path)
    {
        return getKey(path).getInt();
    }
    SInt64 Config::getInt(const String & path, SInt64 default_val)
    {
        return getKey(path,default_val).getInt();
    }

    const String Config::getString(const String & path)
    {
        return getKey(path).getString();
    }
    const String Config::getString(const String & path, const String & default_val)
    {
        return getKey(path,default_val).getString();
    }

    double Config::getFloat(const String & path)
    {
        return getKey(path).getFloat();
    }
    double Config::getFloat(const String & path, double default_val)
    {
        return getKey(path,default_val).getFloat();
    }

}//end of namespace config
