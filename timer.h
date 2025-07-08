#ifndef TIMER_H
#define TIMER_H

#include <sys/time.h>

// Узел в куче таймеров
typedef struct timer_node_s {
    struct timespec expiry_time; // Время, когда таймер сработает
    struct connection_s *conn;   // Ссылка на соединение
    int heap_index;              // Индекс узла в куче для O(1) удаления
    struct timer_node_s *next;   // Для пула свободных узлов
} timer_node_t;

// Пул узлов таймеров для избежания malloc/free
typedef struct {
    timer_node_t *nodes;         // Массив всех узлов
    timer_node_t *free_head;     // Голова списка свободных узлов
    int capacity;
    int used_count;
} timer_node_pool_t;

// Структура таймера (min-heap)
typedef struct {
    timer_node_t **heap;
    timer_node_pool_t node_pool; // Пул узлов
    int capacity;
    int size;
} timer_heap_t;


// API для управления пулом узлов
int timer_node_pool_init(timer_node_pool_t *pool, int capacity);
void timer_node_pool_destroy(timer_node_pool_t *pool);
timer_node_t *timer_node_pool_get(timer_node_pool_t *pool);
void timer_node_pool_release(timer_node_pool_t *pool, timer_node_t *node);

// API для управления таймерами
int timer_heap_init(timer_heap_t *th, int capacity);
void timer_heap_destroy(timer_heap_t *th);
int timer_heap_add(timer_heap_t *th, struct connection_s *conn, int timeout_ms);
void timer_heap_remove(timer_heap_t *th, struct connection_s *conn);
int timer_heap_get_next_timeout(timer_heap_t *th);
void timer_heap_process_expired(timer_heap_t *th);

#endif // TIMER_H
