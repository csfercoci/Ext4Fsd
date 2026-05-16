#ifndef _JBD2_COMPAT_H
#define _JBD2_COMPAT_H

#ifndef assert
#define assert(x) ASSERT(x)
#endif

#define pr_emerg(...) printk(KERN_EMERG __VA_ARGS__)
#define pr_err(...) printk(KERN_ERR __VA_ARGS__)
#define ZERO_OR_NULL_PTR(x) ((x) == NULL)

typedef u64 ktime_t;
static inline ktime_t ktime_get(void) {
    LARGE_INTEGER li;
    KeQuerySystemTimePrecise(&li);
    return (ktime_t)(li.QuadPart * 100);
}
static inline u64 ktime_to_ns(ktime_t t) { return (u64)(t); }
static inline ktime_t ktime_sub(ktime_t a, ktime_t b) { return (a) - (b); }
static inline ktime_t ktime_add_ns(ktime_t t, u64 ns) { return (t) + (ns); }
#define HRTIMER_MODE_ABS 0
static inline void schedule_hrtimeout(ktime_t *expires, int mode) { }

#define round_jiffies_up(x) (x)
static inline void add_timer(void *t) { }

#define read_lock(lock)
#define read_unlock(lock)
#define write_lock(lock)
#define write_unlock(lock)

typedef int rwlock_t;
struct rw_semaphore { int dummy; };

struct lock_class_key { int dummy; };
#define lockdep_set_class(lock, key)

#define rwsem_acquire(lock, subclass, trylock, ip)
#define rwsem_release(lock, nested, ip)
#define rwsem_acquire_read(lock, subclass, trylock, ip)

#define schedule()
#define wait_event(wq, condition)

#define _THIS_IP_ 0

static inline void *kmem_cache_zalloc(kmem_cache_t *c, int flags) {
    void *p = kmem_cache_alloc(c, flags);
    if (p) RtlZeroMemory(p, c->size);
    return p;
}

#define memalloc_nofs_save() 0
#define memalloc_nofs_restore(x)

#define trace_jbd2_handle_start(...)
#define trace_jbd2_handle_extend(...)
#define trace_jbd2_handle_stats(...)
#define trace_jbd2_lock_buffer_stall(...)

#define offset_in_page(x) ((unsigned long)(x) & (PAGE_SIZE - 1))
#define kmap_atomic(page) page_address(page)
#define kunmap_atomic(addr)
#define jiffies_to_msecs(x) (x)

#define smp_wmb() KeMemoryBarrier()
#define smp_mb() KeMemoryBarrier()
#define wait_on_bit_io(...)
#define rcu_read_lock()
#define rcu_read_unlock()
#define READ_ONCE(x) (x)

#ifndef max_t
#define max_t(type, x, y) ((type)(x) > (type)(y) ? (type)(x) : (type)(y))
#endif

#ifndef min_t
#define min_t(type, x, y) ((type)(x) < (type)(y) ? (type)(x) : (type)(y))
#endif

#define PF_MEMALLOC 0

#define PagePrivate(page) 0
#define page_private(page) 0
#define filemap_fdatawrite_range(...) 0

static inline int __warn_on_once(int cond) {
    if (cond) { assert(!(cond)); }
    return cond;
}
#define WARN_ON_ONCE(x) __warn_on_once(!!(x))
#define atomic_add_return(i, v) (atomic_add(i, v), atomic_read(v))

#define J_EXPECT_JH(jh, expr, why) (expr)

#ifndef true
#define true 1
#endif

#ifndef false
#define false 0
#endif

#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
#endif

#define KMEM_CACHE(__type, __flags) \
    kmem_cache_create(#__type, sizeof(struct __type), \
                      __alignof(struct __type), \
                      (__flags), NULL)

#define kmalloc_array(n, size, flags) \
    ((size) != 0 && (n) != 0 && (size) > (~(size_t)0) / (n) ? NULL : \
     kmalloc((n) * (size), flags))

static inline void *jbd2_alloc(size_t size, gfp_t flags) {
    return kmalloc(size, flags);
}

static inline void jbd2_free(void *ptr, size_t size) {
    kfree(ptr);
}

static inline int try_to_free_buffers(struct page *page) {
    return 0;
}

#endif
