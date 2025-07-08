#ifndef LOCKFREE_POOL_H
#define LOCKFREE_POOL_H

#include "connection.h"
#include <stdatomic.h>
#include <stdint.h>

// Lock-free connection pool для максимальной производительности
// Каждый CPU core имеет свой локальный пул для избежания contention

#define MAX_CPU_CORES 32
#define CONNECTIONS_PER_CORE 512
#define TOTAL_CONNECTIONS (MAX_CPU_CORES * CONNECTIONS_PER_CORE)

// Per-CPU connection pool
typedef struct {
    connection_t connections[CONNECTIONS_PER_CORE];
    
    // Lock-free stack для свободных соединений
    atomic_int free_stack[CONNECTIONS_PER_CORE];
    atomic_int free_top;
    
    // Статистика для мониторинга
    atomic_int used_count;
    atomic_int peak_usage;
    atomic_long total_allocations;
    atomic_long total_deallocations;
    
    // Padding для избежания false sharing
    char padding[64 - (sizeof(atomic_int) * 4 + sizeof(atomic_long) * 2) % 64];
} __attribute__((aligned(64))) per_cpu_pool_t;

// Глобальная структура пула
typedef struct {
    per_cpu_pool_t cpu_pools[MAX_CPU_CORES];
    atomic_int active_cores;
    
    // Fallback pool когда локальный пул исчерпан
    connection_t *global_connections;
    atomic_int global_free_stack[TOTAL_CONNECTIONS];
    atomic_int global_free_top;
    atomic_int global_capacity;
    
    // Статистика
    atomic_long global_allocations;
    atomic_long global_deallocations;
    atomic_long cross_cpu_allocations;
} lockfree_pool_t;

// API функции
int lockfree_pool_init(lockfree_pool_t *pool);
void lockfree_pool_destroy(lockfree_pool_t *pool);
connection_t *lockfree_pool_get(lockfree_pool_t *pool);
void lockfree_pool_release(lockfree_pool_t *pool, connection_t *conn);

// Утилиты для CPU affinity
int get_current_cpu_id(void);
int set_thread_affinity(int cpu_id);
void print_pool_statistics(lockfree_pool_t *pool);

// Inline функции для быстрого доступа
static inline int lockfree_stack_pop(atomic_int *stack, atomic_int *top) {
    int current_top, new_top, value;
    
    do {
        current_top = atomic_load_explicit(top, memory_order_acquire);
        if (current_top < 0) return -1; // Stack empty
        
        value = atomic_load_explicit(&stack[current_top], memory_order_relaxed);
        new_top = current_top - 1;
    } while (!atomic_compare_exchange_weak_explicit(
        top, &current_top, new_top,
        memory_order_release, memory_order_relaxed));
    
    return value;
}

static inline int lockfree_stack_push(atomic_int *stack, atomic_int *top, int value, int capacity) {
    int current_top, new_top;
    
    do {
        current_top = atomic_load_explicit(top, memory_order_acquire);
        new_top = current_top + 1;
        if (new_top >= capacity) return -1; // Stack full
        
        atomic_store_explicit(&stack[new_top], value, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
        top, &current_top, new_top,
        memory_order_release, memory_order_relaxed));
    
    return 0;
}

// Memory ordering utilities
#define MEMORY_BARRIER() atomic_thread_fence(memory_order_seq_cst)
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

// Cache line prefetching
static inline void prefetch_connection(connection_t *conn) {
    __builtin_prefetch(conn, 1, 3);
    __builtin_prefetch((char*)conn + 64, 1, 3);
}

// Fast CPU ID detection using RDTSCP if available
#ifdef __x86_64__
static inline int fast_get_cpu_id(void) {
    unsigned int cpu_id;
    __asm__ __volatile__("rdtscp" : "=c"(cpu_id) :: "rax", "rdx");
    return cpu_id & 0xFF;
}
#else
#define fast_get_cpu_id() get_current_cpu_id()
#endif

// Branch prediction hints
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// Connection state validation
static inline int is_valid_connection(connection_t *conn, lockfree_pool_t *pool) {
    // Check if connection pointer is within valid ranges
    for (int i = 0; i < MAX_CPU_CORES; i++) {
        per_cpu_pool_t *cpu_pool = &pool->cpu_pools[i];
        if (conn >= cpu_pool->connections && 
            conn < cpu_pool->connections + CONNECTIONS_PER_CORE) {
            return 1;
        }
    }
    
    // Check global pool
    if (pool->global_connections && 
        conn >= pool->global_connections && 
        conn < pool->global_connections + pool->global_capacity) {
        return 1;
    }
    
    return 0;
}

// Performance monitoring
typedef struct {
    uint64_t allocations_per_second;
    uint64_t deallocations_per_second;
    double average_pool_utilization;
    double cross_cpu_allocation_ratio;
    uint64_t cache_misses_estimated;
} pool_performance_stats_t;

void get_pool_performance_stats(lockfree_pool_t *pool, pool_performance_stats_t *stats);

#endif // LOCKFREE_POOL_H