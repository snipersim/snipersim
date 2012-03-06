#ifndef TLS_H
#define TLS_H

#include "fixed_types.h"

class TLS
{
public:
    virtual ~TLS();

    virtual void* get(int thread_id = -1) = 0;
    virtual const void* get(int thread_id = -1) const = 0;

    template<class T>
        T& get(int thread_id = -1) { return *((T*)get(thread_id)); }

    template<class T>
        const T& get(int thread_id = -1) const { return *((const T*)get(thread_id)); }

    template<class T>
        T* getPtr(int thread_id = -1) { return (T*)get(thread_id); }

    template<class T>
        const T* getPtr(int thread_id = -1) const { return (const T*)get(thread_id); }

    IntPtr getInt(int thread_id = -1) const { return (intptr_t)get(thread_id); }

    virtual void set(void *) = 0;

    template<class T>
        void set(T *p) { set((void*)p); }

    void setInt(IntPtr i) { return set((void*)i); }

    static TLS* create();

protected:
    TLS();
};

#endif // TLS_H
