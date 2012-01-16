#include <sys/syscall.h>

#include "tls.h"
#include "log.h"

/*
  HashTLS implements TLS via a large hash map that we assume will
  never have collisions (in order for there to be collisions, two
  thread ids within the same process would have to differ by
  HASH_SIZE).

  If PinTLS is ever fixed, then HashTLS should probably be replaced by
  PinTLS. PthreadTLS is certainly safer when Pin is not being used.
*/

class HashTLS : public TLS
{
public:
    HashTLS()
    {
    }

    ~HashTLS()
    {
    }

    int getTid()
    {
        #if 1
            int tid = syscall(__NR_gettid);
        #else
            /* Since we're called on every PIN callback (more than once per simulated instruction),
               the gettid() syscall is a large overhead (~440ns per call, 20% for the simple cpu model).
               Since we just need a per-thread unique id, we can derive this from the current stack pointer. */
            int t;
            int esp = (long)&t;
            /* TODO: the >>16 isn't guaranteed to always give a one-to-one mapping to the threadid. Fix. */
            int tid = esp >> 16;
        #endif
        return tid;
    }

    void* get(int thread_id)
    {
        int tid = getTid();
        return m_data[tid % HASH_SIZE];
    }

    const void* get(int thread_id) const
    {
        return ((HashTLS*)this)->get(thread_id);
    }

    void set(void *vp)
    {
        int tid = getTid();
        m_data[tid % HASH_SIZE] = vp;
    }

private:

    static const int HASH_SIZE = 10007; // prime
    void *m_data[HASH_SIZE];
};

__attribute__((weak)) TLS* TLS::create()
{
    return new HashTLS();
}
