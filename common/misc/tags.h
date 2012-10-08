#ifndef __TAGS_H__
#define __TAGS_H__

#include "fixed_types.h"

#include <string>
#include <map>

namespace config
{
   class Config;
}

// Tags
//
//   An object (String) can have a number of tags (String) associated with it. Additionally, each tag can be assigned
// a value (UInt64). The goal of this tagging system is to allow for informal communication between different components.
// Initial tags are set via init() from the configuration file.
//
//  [tags]
//  objname/tagname=1,0,1,0
//
struct Tag {
   String tag;
   UInt64 value;

   Tag(String tag, UInt64 value) : tag(tag), value(value) {}

   bool operator<(const Tag & rhs) const
   {
      if (tag == rhs.tag)
      {
         return value < rhs.value;
      }
      else
      {
         return tag < rhs.tag;
      }
   }

   bool operator==(const Tag & rhs) const
   {
      return ((this->tag == rhs.tag) && (this->value == rhs.value));
   }
};

class TagsManager
{
public:
   TagsManager(config::Config *config);

   void addTag(String objname, UInt64 id, String tag, UInt64 value = 1)
   {
      m_tags[objname][Tag(tag,id)] = value;
   }

   void updateTag(String objname, UInt64 id, String tag, UInt64 value = 1)
   {
      addTag(objname,id,tag,value);
   }

   bool hasTag(String objname, UInt64 id, String tag)
   {
      return (m_tags[objname].count(Tag(tag,id)) == 1);
   }

   UInt64 getTag(String objname, UInt64 id, String tag)
   {
      return m_tags[objname][Tag(tag,id)];
   }

   void deleteTag(String objname, UInt64 id, String tag)
   {
      m_tags[objname].erase(Tag(tag,id));
   }

private:
   // FIXME: An unordered_map should be faster than a map
   std::map<String, std::map<Tag,UInt64> > m_tags;
};

#endif /* __TAGS_H__ */
