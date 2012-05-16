// Jonathan Eastep
// Build up / consume unstructured packets through a simple interface

#ifndef PACKETIZE_H
#define PACKETIZE_H

#include "log.h"
#include "fixed_types.h"
#include "subsecond_time.h"

//#define DEBUG_UNSTRUCTURED_BUFFER
#include <assert.h>
#include <iostream>
#include <utility>

class UnstructuredBuffer
{

private:
    String m_chars;

public:

    UnstructuredBuffer();
    const void* getBuffer();
    void clear();
    int size();

    // These put / get scalars
    template<class T> void put(const T & data);
    template<class T> bool get(T& data);

    // These put / get arrays
    // Note: these are shallow copy only
    template<class T> void put(const T * data, int num);
    template<class T> bool get(T* data, int num);

    // Wrappers
    template<class T>
    UnstructuredBuffer& operator<<(const T & data);
    template<class T>
    UnstructuredBuffer& operator>>(T & data);

    template<class T, class I>
    UnstructuredBuffer& operator<<(std::pair<T*, I> buffer);
    template<class T, class I>
    UnstructuredBuffer& operator>>(std::pair<T*, I> buffer);

    UnstructuredBuffer& operator<<(std::pair<const void*, int> buffer);
    UnstructuredBuffer& operator>>(std::pair<void*, int> buffer);
};

template<class T> void UnstructuredBuffer::put(const T* data, int num)
{
    assert(num >= 0);
    m_chars.append((const char *) data, num * sizeof(T));
}

template<class T> bool UnstructuredBuffer::get(T* data, int num)
{
    assert(num >= 0);
    if (m_chars.size() < (num * sizeof(T)))
        return false;

    m_chars.copy((char *) data, num * sizeof(T));
    m_chars.erase(0, num * sizeof(T));

    return true;
}

template<class T> void UnstructuredBuffer::put(const T& data)
{
    put<T>(&data, 1);
}

template<> inline void UnstructuredBuffer::put(const SubsecondTime& _data)
{
    subsecond_time_t data;
    data = _data;
    put<subsecond_time_t>(&data, 1);
}

template<class T> bool UnstructuredBuffer::get(T& data)
{
    return get<T>(&data, 1);
}

template<> inline bool UnstructuredBuffer::get(SubsecondTime& _data)
{
    subsecond_time_t data;
    bool res = get<subsecond_time_t>(&data, 1);
    _data = data;
    return res;
}

template<class T>
UnstructuredBuffer& UnstructuredBuffer::operator<<(const T & data)
{
    put<T>(data);
    return *this;
}

// Specialize for SubsecondTime
template<>
inline UnstructuredBuffer& UnstructuredBuffer::operator<<(const SubsecondTime & _data)
{
    subsecond_time_t data = _data;
    put<subsecond_time_t>(data);
    return *this;
}

template<class T>
UnstructuredBuffer& UnstructuredBuffer::operator>>(T & data)
{
    __attribute__((unused)) bool res = get<T>(data);
    assert(res);
    return *this;
}

// Specialize for SubsecondTime
template<>
inline UnstructuredBuffer& UnstructuredBuffer::operator>>(SubsecondTime & _data)
{
    subsecond_time_t data;
    __attribute__((unused)) bool res = get<subsecond_time_t>(data);
    assert(res);
    _data = data;
    return *this;
}

template<class T, class I>
UnstructuredBuffer& UnstructuredBuffer::operator<<(std::pair<T*, I> buffer)
{
    return (*this) << std::make_pair((const void *) buffer.first, (int) buffer.second);
}

template<class T, class I>
UnstructuredBuffer& UnstructuredBuffer::operator>>(std::pair<T*, I> buffer)
{
    return (*this) >> std::make_pair((void*) buffer.first, (int) buffer.second);
}

#endif
