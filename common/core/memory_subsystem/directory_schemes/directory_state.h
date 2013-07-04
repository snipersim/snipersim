#ifndef __DIRECTORY_STATE_H__
#define __DIRECTORY_STATE_H__

class DirectoryState
{
   public:
      enum dstate_t
      {
         UNCACHED = 0,
         SHARED,
         OWNED,
         MODIFIED,
         EXCLUSIVE,
         NUM_DIRECTORY_STATES
      };
};

#endif /* __DIRECTORY_STATE_H__ */
