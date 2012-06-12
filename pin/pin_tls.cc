#include "tls.h"
#include "log.h"
#include <pin.H>

class PinTLS : public TLS
{
public:
    PinTLS()
    {
        m_key = PIN_CreateThreadDataKey(NULL);
    }

    ~PinTLS()
    {
        PIN_DeleteThreadDataKey(m_key);
    }

    void* get(int thread_id)
    {
        if (thread_id == -1)
            return PIN_GetThreadData(m_key, PIN_ThreadId());
        else
            return PIN_GetThreadData(m_key, thread_id);
    }

    const void* get(int thread_id) const
    {
        return ((PinTLS*)this)->get(thread_id);
    }

    void set(void *vp)
    {
        LOG_PRINT("%p->set(%p)", this, vp);
        __attribute__((unused)) BOOL res = PIN_SetThreadData(m_key, vp, PIN_ThreadId());
        LOG_ASSERT_ERROR(res, "Error setting TLS -- pin tid = %d", PIN_ThreadId());
    }

private:
    TLS_KEY m_key;
};

#if 1
// override PthreadTLS
TLS* TLS::create()
{
    return new PinTLS();
}
#endif
