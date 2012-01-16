#ifndef CONFIG_HPP
#define CONFIG_HPP
/*!
 *\file config.hpp
 * This file contains most of the interface for the external->in memory configuration management interface.
 * There is a Config class which is the main interface to the outside world for loading files, getting and
 * setting values as well as saving the given configuration. This base class is then derived from for
 * the different given backends. This Config class has a root 'Section'. Each section has a list of
 * subsections and a list of keys. Each key actually contains the value.
 */
// Config Class
// Author: Charles Gruenwald III
#include <vector>
#include <map>
#include <iostream>

#include "fixed_types.h"

#include "key.hpp"
#include "section.hpp"
#include "config_exceptions.hpp"


namespace config
{

    //
    typedef std::vector < String > PathElementList;
    typedef std::pair<String,String> PathPair;

    /*! \brief Config: A class for managing the interface to persistent configuration entries defined at runtime.
     * This class is used to manage a configuration interface.
     * It is the base class for which different back ends will derive from.
     */
    class Config
    {
        public:
            Config(bool case_sensitive = false): m_case_sensitive(case_sensitive), m_root("", case_sensitive){}
            Config(const Section & root, bool case_sensitive = false): m_case_sensitive(case_sensitive), m_root(root, "", case_sensitive){}
            virtual ~Config(){}

            /*! \brief A function for saving the entire configuration
             * tree to the specified path.
             * This function is responsible for walking through the entire
             * configuration tree (starting at the root) and outputing an
             * appropriate text representation than can then be re-read by
             * an appropriate configuration class.
             * Note: In the case of a write-through situation (as is the case
             * with the windows registry), this function is unnecessary.
             */
            virtual void saveAs(const String & path){}

            /*! \brief A function to convert from external representation to in-memory representation
             * This function sets the member variable m_path and then calls the loadConfig() function which
             * must be implemented by the derived back-end implementation.
             * \param path - This is the path where we load the configuration from. This may either be a registry
             * path for the case of the ConfigRegistry interface or a file path in the case of a ConfigFile interface.
             * \exception FileNotFound - This exception is thrown if the ConfigFile interface is instructed to load
             * a file which does not exist.
             * \exception parseError - This exception is thrown on a malformed file with the ConfigFile interface.
             */
            void load(const String &path);

            /*! \brief A function which will save the in-memory representation to the external
             * representation that was specified with the load() function.
             */
            void Save(){saveAs(m_path);}

            void clear();

            //! A function that will save a given value to key at the specified path.
            virtual void set(const String & path, const String & new_value);

            /*!
             * \overload virtual void set(const String & path, SInt64 new_value);
             */
            virtual void set(const String & path, SInt64 new_value);

            /*!
             * \overload virtual void set(const String & path, double new_value);
             */
            virtual void set(const String & path, double new_value);

            //! Returns a reference to the section at the given path.
            const Section & getSection(const String & path);


            //! Returns a reference to the root section of the configuration tree.
            const Section & getRoot() {return m_root;}

            /*! \brief addSection() adds the specified path as a new section, creating each entry
             * in the path along the way.
             */
            const Section & addSection(const String & path);


            /*! \brief Look up the key at the given path, and return the value of that key as a bool.
             * \param path - Path for key to look up
             * \exception KeyNotFound is thrown if the specified path doesn't exist.
             */
            bool getBool(const String & path);

            /*! \brief Look up the key at the given path, return default_val if not found.
             * \param path - Path for key to look up
             * \param default_val - Value to return if the specified key is not found.
             */
            bool getBool(const String & path, bool default_val);

            /*! \brief Look up the key at the given path, return default_val if not found.
             * \param path - Path for key to look up
             * \param default_val - Value to return if the specified key is not found.
             */
            bool getBool(const String & path, const String & default_val);

            /*! \brief Look up the key at the given path, return default_val if not found.
             * \param path - Path for key to look up
             * \param default_val - Value to return if the specified key is not found.
             * This is a special case to prevent conversion from char* to bool by default
             */
            bool getBool(const String & path, const char * default_val);

            /*! \brief Look up the key at the given path, and return the value of that key as a bool.
             * \param path - Path for key to look up
             * \exception KeyNotFound is thrown if the specified path doesn't exist.
             */
            SInt64 getInt(const String & path);

            /*! \brief Look up the key at the given path, return default_val if not found.
             * \param path - Path for key to look up
             * \param default_val - Value to return if the specified key is not found.
             */
            SInt64 getInt(const String & path, SInt64 default_val);

            /*! \brief Look up the key at the given path, and return the value of that key as a bool.
             * \param path - Path for key to look up
             * \exception KeyNotFound is thrown if the specified path doesn't exist.
             */
            const String getString(const String & path);

            /*! \brief Look up the key at the given path, return default_val if not found.
             * \param path - Path for key to look up
             * \param default_val - Value to return if the specified key is not found.
             */
            const String getString(const String & path, const String & default_val);

            //! Same as getString()
            const String get(const String &path) { return getString(path); }
            //! Same as getString()
            const String get(const String &path, const String & default_val) { return getString(path, default_val); }

            /*! \brief Look up the key at the given path, and return the value of that key as a bool.
             * \param path - Path for key to look up
             * \exception KeyNotFound is thrown if the specified path doesn't exist.
             */
            double getFloat(const String & path);

            /*! \brief Look up the key at the given path, return default_val if not found.
             * \param path - Path for key to look up
             * \param default_val - Value to return if the specified key is not found.
             */
            double getFloat(const String & path, double default_val);

            /*! \brief Returns a string representation of the tree starting at the specified section
             * \param current The root of the tree for which we are creating a string representation.
             */
            String showTree(const Section & current, int depth = 0);

            //! \brief Returns a string representation of the loaded configuration tree
            String showFullTree() { return showTree(m_root); }

            /*! addKey() Adds the specified path as a new key (with the given value), creating each entry
             * in the path along the way.
             * \param path The path to the new key
             * \param new_key The value for the newly created key
             */
            const Key & addKey(const String & path, const String & new_key);
            /*! \overload addKey()
             */
            const Key & addKey(const String & path, const SInt64 new_key);
            /*! \overload addKey()
             */
            const Key & addKey(const String & path, const double new_key);

        protected:
            bool m_case_sensitive;
            Section m_root;
            String m_path;
            virtual void loadConfig() = 0;

            Section & getSection_unsafe(String const& path);
            Section & getRoot_unsafe() {return m_root; };
            Key & getKey_unsafe(String const& path);

        private:
            const Key & getKey(const String & path);
            const Key & getKey(const String & path, SInt64 default_val);
            const Key & getKey(const String & path, double default_val);
            const Key & getKey(const String & path, const String &default_val);

            //Utility function used to break the last word past the last /
            //from the base path
            PathPair splitPath(const String & path);

            //This function splits the path up like the function above, but also allows you to
            //pass the path_elements vector in (for later traversal)
            PathPair splitPathElements(const String & path, PathElementList & path_elements);

            //Utility function to determine if a given path is a leaf (i.e. it has no '/'s in it)
            bool isLeaf(const String & path);
    };

}//end of namespace config

#endif //BL_CONFIG_HPP
