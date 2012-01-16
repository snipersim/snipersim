#ifndef __SPIN_LOCK_H__
#define __SPIN_LOCK_H__

typedef struct {
	volatile unsigned int slock;
} raw_spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED	{ 1 }/*
 * Your basic SMP spinlocks, allowing only a single CPU anywhere
 *
 * Simple spin lock operations.  There are two variants, one clears IRQ's
 * on the local processor, one does not.
 *
 * We make no fairness assumptions. They have a cost.
 *
 * (the type definitions are in asm/spinlock_types.h)
 */

#define __raw_spin_is_locked(x) \
		(*(volatile signed char *)(&(x)->slock) <= 0)

#define __raw_spin_lock_string \
	"\n1:\t" \
	"lock ; decb %0\n\t" \
	"jns 3f\n" \
	"2:\t" \
	"rep;nop\n\t" \
	"cmpb $0,%0\n\t" \
	"jle 2b\n\t" \
	"jmp 1b\n" \
	"3:\n\t"

/*
 * NOTE: there's an irqs-on section here, which normally would have to be
 * irq-traced, but on CONFIG_TRACE_IRQFLAGS we never use
 * __raw_spin_lock_string_flags().
 */
#define __raw_spin_lock_string_flags \
	"\n1:\t" \
	"lock ; decb %0\n\t" \
	"jns 5f\n" \
	"2:\t" \
	"testl $0x200, %1\n\t" \
	"jz 4f\n\t" \
	"sti\n" \
	"3:\t" \
	"rep;nop\n\t" \
	"cmpb $0, %0\n\t" \
	"jle 3b\n\t" \
	"cli\n\t" \
	"jmp 1b\n" \
	"4:\t" \
	"rep;nop\n\t" \
	"cmpb $0, %0\n\t" \
	"jg 1b\n\t" \
	"jmp 4b\n" \
	"5:\n\t"

static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
	asm(__raw_spin_lock_string : "+m" (lock->slock) : : "memory");
}

/*
 * It is easier for the lock validator if interrupts are not re-enabled
 * in the middle of a lock-acquire. This is a performance feature anyway
 * so we turn it off:
 */
#ifndef CONFIG_PROVE_LOCKING
static inline void __raw_spin_lock_flags(raw_spinlock_t *lock, unsigned long flags)
{
	asm(__raw_spin_lock_string_flags : "+m" (lock->slock) : "r" (flags) : "memory");
}
#endif

static inline int __raw_spin_trylock(raw_spinlock_t *lock)
{
	char oldval;
	__asm__ __volatile__(
		"xchgb %b0,%1"
		:"=q" (oldval), "+m" (lock->slock)
		:"0" (0) : "memory");
	return oldval > 0;
}

/*
 * __raw_spin_unlock based on writing $1 to the low byte.
 * This method works. Despite all the confusion.
 * (except on PPro SMP or if we are using OOSTORE, so we use xchgb there)
 * (PPro errata 66, 92)
 */

#if !defined(CONFIG_X86_OOSTORE) && !defined(CONFIG_X86_PPRO_FENCE)

#define __raw_spin_unlock_string \
	"movb $1,%0" \
		:"+m" (lock->slock) : : "memory"


static inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
	__asm__ __volatile__(
		__raw_spin_unlock_string
	);
}

#else

#define __raw_spin_unlock_string \
	"xchgb %b0, %1" \
		:"=q" (oldval), "+m" (lock->slock) \
		:"0" (oldval) : "memory"

static inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
	char oldval = 1;

	__asm__ __volatile__(
		__raw_spin_unlock_string
	);
}

#endif

#define __raw_spin_unlock_wait(lock) \
	do { while (__raw_spin_is_locked(lock)) cpu_relax(); } while (0)

#endif /* __SPIN_LOCK_H__ */
