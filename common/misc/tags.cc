#include "tags.h"

#include "simulator.h"
#include "config.hpp"

#include <iostream>

// Parse initial tag values from the configuration
// Example:
//
//  [tags]
//  objname/tagname=1,0,1,0
//
// The tags gets added to the obj only if it is valid (Key::getBool() == true)
// We currently do not support assigning tag values via the configuration variables,
//   but it's value is initialized to 1.
//
TagsManager::TagsManager(config::Config *config)
{
   const config::Section &section = config->getSection("tags");
   const config::SectionList &objs = section.getSubsections();

   for (config::SectionList::const_iterator obj = objs.begin() ; obj != objs.end() ; ++obj)
   {
      String objname = (*obj).first;

      // Default configuration values are not supported, only array lists
      const config::KeyArrayList &tags_keys = (*obj).second->getArrayKeys();
      for (config::KeyArrayList::const_iterator tag_keys = tags_keys.begin() ; tag_keys != tags_keys.end() ; ++tag_keys)
      {
         const String &tag = (*tag_keys).first;
         const std::vector<config::Key*> ids = (*tag_keys).second;

         for (unsigned int id = 0 ; id < ids.size() ; id++)
         {
            bool valid = ids[id]->getBool();

            if (valid)
               addTag(objname, id, tag, valid);
         }
      }
   }
}
