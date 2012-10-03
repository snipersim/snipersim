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
void TagsManager::init()
{
   const config::Section &section = Sim()->getCfg()->getSection("tags");
   const config::SectionList &objs = section.getSubsections();
   for (auto obj = objs.begin() ; obj != objs.end() ; ++obj)
   {
      String objname = (*obj).first;

      // Default configuration values are not supported, only array lists
      const config::KeyArrayList &tags_keys = (*obj).second->getArrayKeys();
      for (auto tag_keys = tags_keys.begin() ; tag_keys != tags_keys.end() ; ++tag_keys)
      {
         const String &tag = (*tag_keys).first;
         const std::vector<boost::shared_ptr<config::Key> > ids = (*tag_keys).second;

         for (unsigned int id = 0 ; id < ids.size() ; id++)
         {
            bool valid = ids[id]->getBool();
            TagsManager *tm = Sim()->getTagsManager();

            if (valid)
               tm->addTag(objname, id, tag, valid);
         }
      }
   }
}
