#include "tls.h"
#include <pthread.h>

// Pthread

class PthreadTLS : public TLS
{
public:
    PthreadTLS()
    {
        pthread_key_create(&m_key, NULL);
    }

    ~PthreadTLS()
    {
        pthread_key_delete(m_key);
    }

    void* get(int thread_id = -1)
    {
        return pthread_getspecific(m_key);
    }

    const void* get(int thread_id = -1) const
    {
        return pthread_getspecific(m_key);
    }

    void set(void *vp)
    {
        pthread_setspecific(m_key, vp);
    }

private:
    pthread_key_t m_key;
};

__attribute__((weak)) TLS* TLS::create()
{
    return new PthreadTLS();
}
