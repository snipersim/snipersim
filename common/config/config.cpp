/*!
 *\file
*/
// Config Class
// Author: Charles Gruenwald III

#include "config.hpp"

#include <boost/algorithm/string.hpp>
#include <cstdarg>
#include <cstdio>

namespace config
{

   void Error(const char* format, ...)
   {
      fprintf(stderr, "\n*** Configuration error ***\n");
      va_list args;
      va_start(args, format);
      vfprintf(stderr, format, args);
      va_end(args);
      fprintf(stderr, "\n\n");
      exit(0);
   }

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

    bool Config::hasKey(const String & path, UInt64 index)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(m_root.hasKey(path, index))
                return true;
            else
                return false;
        }

        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section & section = getSection_unsafe(path_pair.first);

        if(section.hasKey(path_pair.second, index))
            return true;
        else
            return false;
    }

    const Key & Config::getKey(const String & path, UInt64 index)
    {
        //Handle the base case
        if(isLeaf(path))
        {
            if(!m_root.hasKey(path, index))
            {
                if (index == UINT64_MAX)
                    config::Error("Configuration value %s not found.", path.c_str());
                else
                    config::Error("Configuration value %s[%i] not found.", path.c_str(), index);
            }
            else
            {
                return m_root.getKey(path, index);
            }
        }

        //Disect the path
        PathPair path_pair = Config::splitPath(path);
        Section & section = getSection_unsafe(path_pair.first);

        if(!section.hasKey(path_pair.second, index))
        {
            if (index == UINT64_MAX)
                config::Error("Configuration value %s not found.", path.c_str());
            else
                config::Error("Configuration value %s[%i] not found.", path.c_str(), index);
        }

        return section.getKey(path_pair.second, index);
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

    template <class V>
    const Key & Config::addKeyInternal(const String & path, const V & value, UInt64 index)
    {
        //Handle the base case
        if(isLeaf(path))
            return m_root.addKey(path, value, index);

        PathPair path_pair = Config::splitPath(path);
        Section &parent = getSection_unsafe(path_pair.first);
        return parent.addKey(path_pair.second, value, index);
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
            Section const & subsection = *(i->second);
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
    bool Config::getBoolArray(const String & path, UInt64 index)
    {
        return getKey(path, index).getBool();
    }

    SInt64 Config::getIntArray(const String & path, UInt64 index)
    {
        return getKey(path, index).getInt();
    }

    const String Config::getStringArray(const String & path, UInt64 index)
    {
        return getKey(path, index).getString();
    }

    double Config::getFloatArray(const String & path, UInt64 index)
    {
        return getKey(path,index).getFloat();
    }

}//end of namespace config
