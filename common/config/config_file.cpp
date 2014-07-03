/*!
 * \file config_file.cpp
 *  The file interface to the config class.
*/
// Config Class
// Author: Charles Gruenwald III
#include "config_file.hpp"
#include "config_exceptions.hpp"
#include "itostr.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <errno.h>

#include <boost/algorithm/string.hpp>
#include <boost/version.hpp>
#include <boost/lexical_cast.hpp>

namespace config
{

    ConfigFile::ConfigFile(bool case_sensitive)
        :
            Config(case_sensitive)
    {
    }

    ConfigFile::ConfigFile(const Section & root, bool case_sensitive)
        :
            Config(root, case_sensitive)
    {
    }

    //load a given filename into a string, taken from boost regexp replace example
    void ConfigFile::loadFileToString(String& s, const String& filename)
    {
        std::ifstream file_in(filename.c_str());

        s.erase();
        s.reserve(file_in.rdbuf()->in_avail());
        char c;
        while(file_in.get(c))
        {
            if(s.capacity() == s.size())
                s.reserve(s.capacity() * 3);
            s.append(1, c);
        }
        file_in.close();
    }

    void ConfigFile::loadConfig()
    {
        String filename = m_path;

        //Make sure the file exists
        if(access(filename.c_str(), R_OK) != 0)
        {
            throw FileNotFound(filename);
        }

        //Read the data in from the file
        String data;
        loadFileToString(data, filename);
        loadConfigFromString(data);

    }

    void ConfigFile::loadConfigFromString(const String & cfg)
    {
        parse(cfg, m_root);
    }


    //Build the in-memory representation by line-by-line matching boost::regex expressions
    void ConfigFile::parse(const String &source, Section & current)
    {
        //rules
        config_parser parser;
        parse_info_t info = ast_parse<config_parser::factory_t>(source.c_str(), source.c_str() + source.size(),parser,space_p | comment_p("#"));

        if(info.full)
        {
            // Uncomment the following lines for more verbose output
//            showParseTree(info.trees.begin());
            evalTree(m_root, info.trees.begin());
        }
        else
        {
            String error_str(info.stop);
            throw parserError(error_str);
        }
    }

    void ConfigFile::showParseTree(tree_iter_t const& node, int depth)
    {
        String tabs = "";
        for(int i=0;i<depth;i++)
            tabs = tabs.append("    ");

        RuleID rule = node->value.value().e;

        // get the string value from the iter
        String value(node->value.begin(), node->value.end() - node->value.begin());
        if(rule == sectionID)
            value = "s: ";

        if(rule == configID)
            value = "c: " + value;

        else if(rule == keyID)
            value = "k: " + value;

        else if(rule == keyValueID)
            value = "kv: " + value;

        else if(rule == keyNameID)
            value = "kn: " + value;

        else if(rule == sectionNameID)
            value = "sn: " + value;

        else if(rule == keyValueArrayID)
            value = "kva: " + value;

        else if(rule == keyValueSpanID)
            value = "kvs: " + value;

        // if(rule == sectionID || rule == keyID || rule == keyValueID || rule == keyNameID || rule == sectionNameID)
        // if(rule > 0)
            std::cout << tabs << (int)(rule) << ".[" << value << "]" << std::endl;

        for(tree_iter_t chi = node->children.begin(); chi != node->children.end(); chi++)
        {
            showParseTree(chi, depth + 1);
        }
    }

    void ConfigFile::unEscapeText(const String & source, String & dest)
    {
        bool backslash = false;
        unsigned int start = 0;
        unsigned int end = source.size();

        //remove surrounding parens if necessary
        if(source.size() >= 2 && source.c_str()[0] == '"' && source.c_str()[source.size() - 1] == '"')
        {
            ++start;
            --end;
        }

        for (unsigned int i = start; i < end; i++) {
            char c = source.c_str()[i];
            if (backslash)
            {
                switch (c)
                {
                    case '\\': dest += "\\"; break;
                    case 'b':  dest += "\b"; break;
                    case 'r':  dest += "\r"; break;
                    case 'f':  dest += "\f"; break;
                    case 't':  dest += "\t"; break;
                    case 'n':  dest += "\n"; break;
                    case '"':  dest += "\""; break;
                    case '\'': dest += "'";  break;
                    default:   dest += c;
                }
                backslash = false;
            }
            else
            {
                if (c == '\\')
                    backslash = true;
                else
                    dest += c;
            }
        }
    }

    String ConfigFile::getNodeValue(tree_iter_t const& node)
    {
        if(node->value.begin() == node->value.end())
            return "";
        return String(node->value.begin(), node->value.end());
    }

    RuleID ConfigFile::getNodeID(tree_iter_t const& node)
    {
        return node->value.value().e;
    }


    void ConfigFile::escapeText(const String & source, String & dest)
    {
        for (unsigned int i = 0; i < source.size(); i++) {
            char c = source.c_str()[i];
                switch (c)
                {
                    case '\\': dest += "\\\\"; break;
                    case '\"': dest += "\\\""; break;
                    default:   dest += c;
                }
        }
    }

    // This function recursively decends the tree built by the parser and populates the config
    // file with the appropriate entries
    void ConfigFile::evalTree(Section & current, tree_iter_t const& node, int depth)
    {
        String tabs = "";
        for(int i=0;i<depth;i++)
            tabs = tabs.append("    ");

        const RuleID rule (getNodeID(node));
        const String value(getNodeValue(node));

        if(rule == sectionNameID)
        {
            // Since this is a  flat file representation, whenever we get a section we only add it
            // to the root node, thus we break out if we are currently nested.
            if(!current.isRoot())
                return;

            //HACK: Strip off the '[' and ']', guaranteed to be matched.
            //This should be removed when we upgrade to Boost 1.35, as per config_file_grammar.hpp
            //note we should just be able to say section_name = value
            String section_name;
            section_name = value.substr(1, value.size() - 2);

            // std::cout << "Found section: [" << section_name << "]" << std::endl;

            // create the section
            Section &child = Config::getSection_unsafe(section_name);

            // add each of the children nodes to this section
            for(tree_iter_t chi_node = node->children.begin(); chi_node != node->children.end(); chi_node++)
            {
                evalTree(child, chi_node, depth + 1);
            }
        }
        else if(rule == keyID)
        {
            //assert(node->children.size() == 1 || node->children.size() == 2);

            String key_name = "";
            String key_value = "";

            // Allow for empty (non-existant) values
            if (node->children.size() != 1 && node->children.size() != 2)
            {
                throw parserError("Internal parser error: Expected one or two keyID children, not " + boost::lexical_cast<String>(node->children.size()));
            }

            if (getNodeID(node->children.begin()) != keyNameID)
            {
                throw parserError("Internal parser error: Expected the first entry to be a keyNameID");
            }

            unEscapeText(getNodeValue(node->children.begin()), key_name);

            if (node->children.size() == 1)
            {
                current.addKey(key_name, key_value);
            }
            else if (node->children.size() == 2)
            {
                UInt64 index = 0;
                tree_iter_t const& datanode = node->children.begin() + 1;
                switch(getNodeID(datanode))
                {
                    case keyValueID:
                        unEscapeText(getNodeValue(datanode), key_value);
                        current.addKey(key_name, key_value);
                        break;
                    case keyValueArrayID:
                        for(tree_iter_t chi = datanode->children.begin(); chi != datanode->children.end(); chi++)
                        {
                            switch(getNodeID(chi))
                            {
                                case keySeparatorID:
                                    ++index;
                                    break;
                                case keyValueID:
                                    key_value = "";
                                    unEscapeText(getNodeValue(chi), key_value);
                                    if (key_value != "")
                                        current.addKey(key_name, key_value, index);
                                    break;
                                default:
                                    throw parserError("Internal parser error: Expected separator or value");
                            }
                        }
                        break;
                    // FIXME: Span should properly create a span of numbers
                    case keyValueSpanID:
                    default:
                        throw parserError("Internal parser error: Unexpected node type");
                }
            }
            else
            {
                throw parserError("Internal parser error: Unexpacted number of keyID children found: " + boost::lexical_cast<String>(node->children.size()));
            }
        }
        else
        {
            for(tree_iter_t chi = node->children.begin(); chi != node->children.end(); chi++)
            {
                evalTree(current, chi, depth + 1);
            }
        }

    }

    void ConfigFile::saveAs(const String &path)
    {
        const String tmp_path(path + ".tmp");

        std::ofstream output_file;
        output_file.exceptions ( std::ofstream::eofbit | std::ofstream::failbit | std::ofstream::badbit );

        //Atomic write with error handling
        try
        {
            output_file.open(tmp_path.c_str());
            SaveTreeAs(output_file, m_root);
            output_file.close();
        }
        catch(const std::ofstream::failure & e)
        {
            throw SaveError(e.what());
        }

        unlink(path.c_str());
        int res = rename(tmp_path.c_str(), path.c_str());
        if (res == -1)
        {
            int errsv = errno;
            String error_str("Moving " + tmp_path + " to " + path + ": error(" + itostr(errsv) + ") " + strerror(errsv) + ".");
            throw SaveError(error_str);
        }
    }

    //Dump the tree in a format that is readable by our parser
    void ConfigFile::SaveTreeAs(std::ofstream &out, const Section &current)
    {

        if(!current.isRoot())
        {
            out << "[" << current.getFullPath() << "]" << std::endl;
        }

        // Write out all of the default values
        KeyList const & keys = current.getKeys();
        for(KeyList::const_iterator i = keys.begin(); i != keys.end();i++)
        {

            const String & name = i->second->getName();
            //Quote the name if it has spaces in it
            if(boost::find_first(name, " ") || boost::find_first(name, "\""))
            {
                String escaped_name;
                escapeText(name, escaped_name);
                out << "\"" << escaped_name << "\"";
            }
            else
                out << name;

            //Quote the value if it is a string
            if(i->second->getFloatValid() || i->second->getIntValid())
                out << " = " << i->second->getString() << std::endl;
            else
            {
                String escaped_value;
                escapeText(i->second->getString(), escaped_value);
                out << " = \"" << escaped_value << "\"" << std::endl;
            }

        }

        //Write out the array data that overrides the defaults
        KeyArrayList const & arr = current.getArrayKeys();
        for(KeyArrayList::const_iterator i = arr.begin(); i != arr.end(); ++i)
        {
            for (uint64_t index = 0; index < i->second.size(); ++index)
            {
                // Find a non-default configuration value where we can get the name from
                if (i->second[index])
                {
                    const String & name = i->second[index]->getName();
                    //Quote the name if it has spaces in it
                    if(boost::find_first(name, " ") || boost::find_first(name, "\""))
                    {
                        String escaped_name;
                        escapeText(name, escaped_name);
                        out << "\"" << escaped_name << "\"";
                    }
                    else
                        out << name;
                    break;
                }
            }

            out << "[] = ";

            for (uint64_t index = 0; index < i->second.size(); ++index)
            {
                if (i->second[index])
                {
                    //Quote the value if it is a string
                    if(i->second[index]->getFloatValid() || i->second[index]->getIntValid())
                        out << i->second[index]->getString();
                    else
                    {
                        String escaped_value;
                        escapeText(i->second[index]->getString(), escaped_value);
                        out << "\"" << escaped_value << "\"";
                    }
                }
                // else: default value, print nothing between the ","

                out << ",";
            }

            out << std::endl;
        }

        out << std::endl;

        SectionList const & subsections = current.getSubsections();
        for(SectionList::const_iterator i = subsections.begin(); i != subsections.end(); i++)
        {
            Section const & subsection = *(i->second);
            SaveTreeAs(out, subsection);
        }


        return;
    }

//namespaces
}

