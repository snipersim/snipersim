#ifndef SUBSECOND_TIME_H__
#define SUBSECOND_TIME_H__

#include "fixed_types.h"
#include "lock.h"

// subsecond_time_t struct is used for c-linkage cases
#include "subsecond_time_c.h"

#include <cassert>
#include <iostream>

#define SUBSECOND_TIME_SIMPLE_OSTREAM

class ComponentTime;
class ComponentPeriod;

template <typename T>
struct TimeConverter
{
   static T PStoFS(T ps)
   {
      return ps * T(1000);
   }

   static inline T NStoPS(T ns)
   {
      return ns * T(1000);
   }

   static inline T NStoFS(T ns)
   {
      return PStoFS( NStoPS(ns) );
   }

   static inline T UStoNS(T us)
   {
      return us * T(1000);
   }
};

class SubsecondTime;
template <class T>
inline SubsecondTime operator*(T lhs, const SubsecondTime& rhs);

class SubsecondTime
{
public:
   static const SubsecondTime FS(uint64_t fs = 1) { return fs * SubsecondTime(FS_1); }
   static const SubsecondTime PS(uint64_t ps = 1) { return ps * SubsecondTime(PS_1); }
   static const SubsecondTime NS(uint64_t ns = 1) { return ns * SubsecondTime(NS_1); }
   static const SubsecondTime US(uint64_t us = 1) { return us * SubsecondTime(US_1); }
   static const SubsecondTime MS(uint64_t ms = 1) { return ms * SubsecondTime(MS_1); }
   static const SubsecondTime SEC(uint64_t sec = 1) { return sec * SubsecondTime(SEC_1); }
   static const SubsecondTime Zero() { return SubsecondTime(); }
   static const SubsecondTime MaxTime() { return SubsecondTime(0xffffffffffffffffULL); }

   // Public constructors
   SubsecondTime()
      : m_time(0)
   {}
   SubsecondTime(const SubsecondTime &_time)
      : m_time(_time.m_time)
   {}
   SubsecondTime(uint64_t multiplier, const SubsecondTime &_time)
      : m_time(_time.m_time * multiplier)
   {}
   SubsecondTime(const subsecond_time_t &sstime)
      : m_time(sstime.m_time)
   {}

   UInt64 getFS() const { return m_time / FS_1; }
   UInt64 getPS() const { return m_time / PS_1; }
   UInt64 getNS() const { return m_time / NS_1; }
   UInt64 getUS() const { return m_time / US_1; }
   UInt64 getMS() const { return m_time / MS_1; }
   UInt64 getSEC() const { return m_time / SEC_1; }

   // Public operators

   // From http://www.stackoverflow.com/questions/4421706
   SubsecondTime& operator+=(const SubsecondTime &rhs)
   {
      m_time += rhs.m_time;
      return *this;
   }
   // From http://www.stackoverflow.com/questions/4421706
   SubsecondTime& operator-=(const SubsecondTime &rhs)
   {
      m_time -= rhs.m_time;
      return *this;
   }

   SubsecondTime& operator<<=(uint64_t rhs)
   {
      m_time <<= rhs;
      return *this;
   }

   // From http://www.stackoverflow.com/questions/1751869
   // From http://www.stackoverflow.com/questions/4421706
   template <class T>
   SubsecondTime& operator*=(T rhs)
   {
      m_time *= rhs;
      return *this;
   }

   SubsecondTime operator/(const uint64_t &divisor) const
   {
      return SubsecondTime(m_time / divisor);
   }

   // Implicit conversion operator from SubsecondTime to subsecond_time_t
   //  to allow operations on SubsecondTime
   // From http://www.stackoverflow.com/questions/4421706
   operator subsecond_time_t() const
   {
      subsecond_time_t sstime;
      sstime.m_time = this->m_time;
      return sstime;
   }

   // Not to be used normally
   uint64_t getInternalDataForced(void) const
   {
      return m_time;
   }

   // Not to be used normally
   void setInternalDataForced(uint64_t new_time)
   {
      m_time = new_time;
   }

   // Boolean operators
   friend inline bool operator==(const SubsecondTime& lhs, const SubsecondTime& rhs);
   friend inline bool operator!=(const SubsecondTime& lhs, const SubsecondTime& rhs);
   friend inline bool operator< (const SubsecondTime& lhs, const SubsecondTime& rhs);
   friend inline bool operator<=(const SubsecondTime& lhs, const SubsecondTime& rhs);
   friend inline bool operator>=(const SubsecondTime& lhs, const SubsecondTime& rhs);
   friend inline bool operator> (const SubsecondTime& lhs, const SubsecondTime& rhs);
   friend inline void atomic_add_subsecondtime(SubsecondTime &src_dest, const SubsecondTime &src);

   // Output stream operators
   friend std::ostream &operator<<(std::ostream &os, const SubsecondTime &time);
   friend inline std::ostream &operator<<(std::ostream &os, const ComponentTime &time);

   // From http://www.stackoverflow.com/questions/4421706
   SubsecondTime& operator*=(const SubsecondTime &rhs)
   {
      m_time *= rhs.m_time;
      return *this;
   }
   // From http://www.stackoverflow.com/questions/4421706
   SubsecondTime& operator/=(const SubsecondTime &rhs)
   {
      m_time /= rhs.m_time;
      return *this;
   }
   // Normally I would inline these as non-member functions, (http://www.stackoverflow.com/questions/4421706),
   // but as we would like to enforce protection, we make them member functions
   inline SubsecondTime operator*(const SubsecondTime& rhs)
   {
      SubsecondTime new_time;
      new_time.m_time = this->m_time * rhs.m_time;
      return new_time;
   }
   inline SubsecondTime operator/(const SubsecondTime& rhs)
   {
      SubsecondTime new_time;
      new_time.m_time = this->m_time / rhs.m_time;
      return new_time;
   }
   inline SubsecondTime operator%(const SubsecondTime &rhs)
   {
      SubsecondTime new_time;
      new_time.m_time = this->m_time % rhs.m_time;
      return new_time;
   }

   static inline uint64_t divideRounded(const SubsecondTime& lhs, const SubsecondTime& rhs)
   {
      return (lhs.m_time + ((rhs.m_time/2) + 1)) / rhs.m_time;
   }

private:
   friend class ComponentPeriod;

   // Not to be used normally
   explicit SubsecondTime(uint64_t _time)
      : m_time(_time)
   {}

   // Internally, numbers are calculated in femtoseconds (10^(-15)s)
   // TimeConverter can only be used when constexpr is supported
   static const uint64_t FS_1 = 1;
   static const uint64_t PS_1 = FS_1 * 1000;
   static const uint64_t NS_1 = PS_1 * 1000;
   static const uint64_t US_1 = NS_1 * 1000;
   static const uint64_t MS_1 = US_1 * 1000;
   static const uint64_t SEC_1 = MS_1 * 1000;

   uint64_t m_time;
};

// Define addition between SubsecondTime instances
// From http://www.stackoverflow.com/questions/4421706
inline SubsecondTime operator+(SubsecondTime lhs, const SubsecondTime& rhs)
{
   return (lhs += rhs);
}

// Define subtraction between SubsecondTime instances
// From http://www.stackoverflow.com/questions/4421706
inline SubsecondTime operator-(SubsecondTime lhs, const SubsecondTime& rhs)
{
   return (lhs -= rhs);
}

// Define bitshift-left on SubsecondTime instances
// From http://www.stackoverflow.com/questions/4421706
inline SubsecondTime operator<<(SubsecondTime lhs, uint64_t rhs)
{
   return (lhs <<= rhs);
}

// Define multiplication with SubsecondTime and uint64_t
// http//www.stackoverflow.com/questions/1751869
template <class T>
inline SubsecondTime operator*(SubsecondTime lhs, T rhs)
{
   return (lhs *= rhs);
}

template <class T>
inline SubsecondTime operator*(T lhs, const SubsecondTime& rhs)
{
   return (rhs * lhs);
}

// Boolean operators
// From http://www.stackoverflow.com/questions/4421706
inline bool operator==(const SubsecondTime& lhs, const SubsecondTime& rhs)
{
   return (lhs.m_time == rhs.m_time);
}
inline bool operator!=(const SubsecondTime& lhs, const SubsecondTime& rhs)
{
   return !operator==(lhs,rhs);
}
inline bool operator< (const SubsecondTime& lhs, const SubsecondTime& rhs)
{
   return (lhs.m_time < rhs.m_time);
}
inline bool operator<=(const SubsecondTime& lhs, const SubsecondTime& rhs)
{
   return !operator> (lhs,rhs);
}
inline bool operator>=(const SubsecondTime& lhs, const SubsecondTime& rhs)
{
   return !operator< (lhs,rhs);
}
inline bool operator> (const SubsecondTime& lhs, const SubsecondTime& rhs)
{
   return  operator< (rhs,lhs);
}

inline void atomic_add_subsecondtime(SubsecondTime &src_dest, const SubsecondTime &src)
{
   __sync_fetch_and_add(&src_dest.m_time, src.m_time);
}

// Base period (frequency) of a component.  This class is normally referenced as a pointer in other generating classes
//  below as it's value can change in DVFS scenarios.
class ComponentPeriod
{
public:
   // Public constructors
   ComponentPeriod(const ComponentPeriod &_p)
      : m_period(_p.m_period)
   {}
   // Only construct ComponentPeriods from this function
   static ComponentPeriod fromFreqHz(uint64_t freq_in_hz)
   {
      // All integer operations
      SubsecondTime new_period = SubsecondTime::SEC() / freq_in_hz;
      return ComponentPeriod( new_period );
   }

   void setPeriodFromFreqHz(uint64_t freq_in_hz)
   {
      m_period = SubsecondTime::SEC() / freq_in_hz;
   }

   SubsecondTime getPeriod(void) const { return m_period; }

   UInt64 getPeriodInFreqMHz(void) const
   {
      return SubsecondTime::US_1 / m_period.m_time;
   }

   // From http://www.stackoverflow.com/questions/1751869
   ComponentPeriod& operator*=(uint64_t rhs)
   {
      m_period *= rhs;
      return *this;
   }

   // Implicit conversion operator from ComponentPeriod to SubsecondTime
   //  to allow operations on SubsecondTime
   // From http://www.stackoverflow.com/questions/4421706
   operator SubsecondTime() const
   {
      return m_period;
   }

private:
   friend inline std::ostream &operator<<(std::ostream &os, const ComponentPeriod &period);

   ComponentPeriod()
   {}
   ComponentPeriod(uint64_t _time)
      : m_period(_time)
   {}
   ComponentPeriod(SubsecondTime &_time)
      : m_period(_time)
   {}

   SubsecondTime m_period;
};

inline ComponentPeriod operator*(ComponentPeriod lhs, uint64_t rhs)
{
   return (lhs *= rhs);
}
inline ComponentPeriod operator*(uint64_t lhs, const ComponentPeriod& rhs)
{
   return (rhs * lhs);
}

inline std::ostream &operator<<(std::ostream &os, const ComponentPeriod &period)
{
   return (os << period.m_period);
}


// To be used to connect legacy components with SubsecondTime
class SubsecondTimeCycleConverter {
public:
   SubsecondTimeCycleConverter(const ComponentPeriod *period)
      : m_period(period)
   {}
   SubsecondTime cyclesToSubsecondTime(UInt64 cycles) const
   {
      return (static_cast<const SubsecondTime>(*m_period) * cycles);
   }
   UInt64 subsecondTimeToCycles(SubsecondTime time) const
   {
      // Get the number of native cycles for this component
      return (time / static_cast<const SubsecondTime>(*m_period)).getInternalDataForced();
   }
private:
   SubsecondTimeCycleConverter()
   {}

   const ComponentPeriod *m_period;
};


// This class is used in components that have fixed bandwidths
// Internally, we keep track of BW in us, but the constructor takes ns.
class ComponentBandwidth
{
public:
   ComponentBandwidth(float bw_in_bits_per_ns)
    : m_bw_in_bits_per_us(bw_in_bits_per_ns * TimeConverter<float>::UStoNS(1)) // bits_per_ns * num_ns_per_us = bits_per_us
   {}

   // X bits * microseconds-per-cycle / bits/cycle = microseconds
   // Multiply by the time unit first to keep the integer result above zero
   SubsecondTime getLatency(uint64_t bits_transmitted) const
   {
      return bits_transmitted * SubsecondTime::US() / m_bw_in_bits_per_us;
   }

   // FIXME, this doesn't properly round at the moment
   SubsecondTime getRoundedLatency(uint64_t bits_transmitted) const
   {
      SubsecondTime us = SubsecondTime::US();
      return ( (bits_transmitted * us) + (us/2) ) / m_bw_in_bits_per_us;
   }

   friend inline std::ostream &operator<<(std::ostream &os, const ComponentBandwidth &time);
private:
   uint64_t m_bw_in_bits_per_us;

   // Default constructor not supposed to be used. If we ever get rid of GCC 4.3, make this '= delete' again.
   ComponentBandwidth() {}
};

inline std::ostream &operator<<(std::ostream &os, const ComponentBandwidth &bandwidth)
{
#ifdef SUBSECOND_TIME_SIMPLE_OSTREAM
   return os << bandwidth.m_bw_in_bits_per_us;
#else
   return (os << "CB:" << bandwidth.m_bw_in_bits_per_us << "-bits/NS");
#endif
}

// This class is used in components that have fixed bandwidths per cycle
class ComponentBandwidthPerCycle
{
public:
   ComponentBandwidthPerCycle(const ComponentPeriod *period, uint64_t bw_in_bits_per_cycle)
      : m_period(period)
      , m_bw_in_bits_per_cycle(bw_in_bits_per_cycle)
   {}

   // This should be deleted or private, but to prevent exceptions in constructors, we'll allow it
   ComponentBandwidthPerCycle()
      : m_period(static_cast<const ComponentPeriod*>(NULL))
   {}

   bool isInfinite() const
   {
      return m_bw_in_bits_per_cycle == 0;
   }

   // X bits * femtoseconds-per-cycle / bits/cycle = femtoseconds
   // Multiply by the period first to keep the integer result above zero
   SubsecondTime getLatency(uint64_t bits_transmitted) const
   {
      return bits_transmitted * static_cast<SubsecondTime>(*m_period) / m_bw_in_bits_per_cycle;
   }

   SubsecondTime getRoundedLatency(uint64_t bits_transmitted) const
   {
      SubsecondTime period = static_cast<SubsecondTime>(*m_period);
      return ( (bits_transmitted * period) + (period/2) ) / m_bw_in_bits_per_cycle;
   }
   SubsecondTime getPeriod(void) const
   {
      return static_cast<SubsecondTime>(*m_period);
   }

   friend inline std::ostream &operator<<(std::ostream &os, const ComponentBandwidthPerCycle &time);
private:
   const ComponentPeriod *m_period;
   uint64_t m_bw_in_bits_per_cycle;

};

inline std::ostream &operator<<(std::ostream &os, const ComponentBandwidthPerCycle &bandwidth)
{
#ifdef SUBSECOND_TIME_SIMPLE_OSTREAM
   return (os << bandwidth.m_bw_in_bits_per_cycle);
#else
   return (os << "CBPC:" << bandwidth.m_bw_in_bits_per_cycle << "@" << bandwidth.m_period);
#endif
}

// This class is used in components that have fixed latencies that can be consumed by other components
// One needs to generate a delay that corresponds to the current frequency (period) of the connected
//  component
class ComponentLatency
{
public:
   ComponentLatency(const ComponentPeriod *period, uint64_t fixed_cycle_latency)
      : m_period(period)
      , m_fixed_cycle_latency(fixed_cycle_latency)
   {}

   SubsecondTime getLatency() const
   {
      return static_cast<SubsecondTime>(*m_period) * m_fixed_cycle_latency;
   }
   SubsecondTime getPeriod(void) const
   {
      return static_cast<SubsecondTime>(*m_period);
   }

   // From http://www.stackoverflow.com/questions/1751869
   ComponentLatency& operator+=(uint64_t rhs)
   {
      m_fixed_cycle_latency += rhs;
      return *this;
   }

   friend inline std::ostream &operator<<(std::ostream &os, const ComponentLatency &time);
private:
   const ComponentPeriod *m_period;
   uint64_t m_fixed_cycle_latency;

   ComponentLatency()
      : m_period(static_cast<const ComponentPeriod*>(0))
   {}
};

inline std::ostream &operator<<(std::ostream &os, const ComponentLatency &latency)
{
#ifdef SUBSECOND_TIME_SIMPLE_OSTREAM
   return (os << latency.m_fixed_cycle_latency);
#else
   return (os << "CL:" << latency.m_fixed_cycle_latency << "@" << latency.m_period);
#endif
}

// This class is used in components like the CPU and Shared Memory subsystems where both time and
//  numbers of cycles in latency need to be added to the core time
// Additionally, through getLatencyGenerator(), a temporary cost variable can be created that can
//  eventually be re-merged into this one.  This is helpful if you would like to add cycle latencies
//  but aren't sure if you'd like to commit them directly to this component yet.
class ComponentTime
{
public:
   // For adding cycles to this component
   // The resulting amount of time will depend on the current frequency
   //  this component is set to
   void addCycleLatency(uint64_t num_cycles)
   {
      m_time += (static_cast<SubsecondTime>(*m_period) * num_cycles);
   }
   // This is for adding time that is not necessarily measured by the base component frequency
   void addLatency(SubsecondTime time_to_add)
   {
      m_time += time_to_add;
   }
   void addLatency(const ComponentTime& time_to_add)
   {
      assert(time_to_add.m_period == this->m_period);
      m_time += time_to_add.m_time;
   }
   // We can skip the function names and use operators because this is the default
   // and expected case.  The time scale bases should be verified to be the same
   // From http://www.stackoverflow.com/questions/1751869
   ComponentTime& operator+=(const ComponentTime& rhs)
   {
      assert(rhs.m_period == this->m_period);
      m_time += rhs.m_time;
      return *this;
   }

   void reset()
   {
      m_time = SubsecondTime();
   }

   ComponentTime getLatencyGenerator(void) const
   {
      return ComponentTime(m_period);
   }
   SubsecondTime getElapsedTime(void) const
   {
      return m_time;
   }
   UInt64 getCycleCount(void) const
   {
      return SubsecondTime::divideRounded(m_time, *m_period);
   }
   SubsecondTime getPeriod(void) const
   {
      return static_cast<SubsecondTime>(*m_period);
   }
   void setElapsedTime(SubsecondTime new_time)
   {
      m_time = new_time;
   }
   // Always construct ComponentTime via this constructor
   // A valid ComponentPeriod is needed to add cycles
   ComponentTime(const ComponentPeriod *_base_period)
      : m_period(_base_period)
      , m_time(SubsecondTime::Zero())
   {}
   // Implicit conversion operator from ComponentTime to ComponentPeriod
   //  to allow operations on ComponentPeriod
   // From http://www.stackoverflow.com/questions/4421706
   operator const ComponentPeriod*() const
   {
      return m_period;
   }

   friend inline std::ostream &operator<<(std::ostream &os, const ComponentTime &time);
private:
   const ComponentPeriod *m_period;
   SubsecondTime m_time;

   ComponentTime()
      : m_period(static_cast<const ComponentPeriod*>(0))
      , m_time(SubsecondTime::Zero())
   {}
};

inline std::ostream &operator<<(std::ostream &os, const ComponentTime &time)
{
#ifdef SUBSECOND_TIME_SIMPLE_OSTREAM
   return (os << time.m_time);
#else
   SubsecondTime cur_time = time.m_time;
   ComponentPeriod cur_period = *(time.m_period);
   SubsecondTime cur_cycle = cur_time / cur_period.getPeriod();
   return (os << cur_time << "(" << cur_cycle << "@" << cur_period << ")");
#endif
}

#endif /* SUBSECOND_TIME_H__ */
