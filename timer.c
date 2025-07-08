#include "timer.h"
#include "worker.h" // Для close_connection_from_worker
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Функции-помощники для работы с кучей (min-heap)
static void sift_up(timer_heap_t *th, int index);
static void sift_down(timer_heap_t *th, int index);
static void swap_nodes(timer_heap_t *th, int a, int b);
static void update_heap_index(timer_heap_t *th, int index);

// Функции для управления пулом узлов таймеров
int timer_node_pool_init(timer_node_pool_t *pool, int capacity) {
    pool->nodes = malloc(sizeof(timer_node_t) * capacity);
    if (!pool->nodes) return -1;
    
    pool->capacity = capacity;
    pool->used_count = 0;
    pool->free_head = NULL;
    
    // Инициализируем список свободных узлов
    for (int i = 0; i < capacity; i++) {
        pool->nodes[i].next = pool->free_head;
        pool->free_head = &pool->nodes[i];
    }
    
    return 0;
}

void timer_node_pool_destroy(timer_node_pool_t *pool) {
    free(pool->nodes);
    pool->nodes = NULL;
    pool->free_head = NULL;
    pool->capacity = 0;
    pool->used_count = 0;
}

timer_node_t *timer_node_pool_get(timer_node_pool_t *pool) {
    if (!pool->free_head) return NULL;
    
    timer_node_t *node = pool->free_head;
    pool->free_head = node->next;
    pool->used_count++;
    
    // Очищаем узел
    memset(node, 0, sizeof(timer_node_t));
    node->heap_index = -1;
    
    return node;
}

void timer_node_pool_release(timer_node_pool_t *pool, timer_node_t *node) {
    if (!node) return;
    
    node->next = pool->free_head;
    pool->free_head = node;
    pool->used_count--;
}

int timer_heap_init(timer_heap_t *th, int capacity) {
    th->heap = malloc(sizeof(timer_node_t*) * capacity);
    if (!th->heap) return -1;
    
    if (timer_node_pool_init(&th->node_pool, capacity) != 0) {
        free(th->heap);
        return -1;
    }
    
    th->capacity = capacity;
    th->size = 0;
    return 0;
}

void timer_heap_destroy(timer_heap_t *th) {
    timer_node_pool_destroy(&th->node_pool);
    free(th->heap);
}

int timer_heap_add(timer_heap_t *th, struct connection_s *conn, int timeout_ms) {
    if (th->size >= th->capacity) return -1; // Куча заполнена

    timer_node_t *node = timer_node_pool_get(&th->node_pool);
    if (!node) return -1;

    clock_gettime(CLOCK_MONOTONIC, &node->expiry_time);
    node->expiry_time.tv_sec += timeout_ms / 1000;
    node->expiry_time.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (node->expiry_time.tv_nsec >= 1000000000) {
        node->expiry_time.tv_sec++;
        node->expiry_time.tv_nsec -= 1000000000;
    }

    node->conn = conn;
    node->heap_index = th->size;
    conn->timer_node = node;

    th->heap[th->size] = node;
    sift_up(th, th->size);
    th->size++;

    return 0;
}

void timer_heap_remove(timer_heap_t *th, struct connection_s *conn) {
    if (!conn->timer_node) return;

    timer_node_t *node_to_remove = conn->timer_node;
    int index = node_to_remove->heap_index;
    
    if (index < 0 || index >= th->size || th->heap[index] != node_to_remove) {
        return; // Invalid index or corrupted heap
    }

    th->size--;
    if (index != th->size) {
        th->heap[index] = th->heap[th->size];
        th->heap[index]->heap_index = index;
        
        // Restore heap property
        if (index > 0 && 
            (th->heap[index]->expiry_time.tv_sec < th->heap[(index-1)/2]->expiry_time.tv_sec ||
             (th->heap[index]->expiry_time.tv_sec == th->heap[(index-1)/2]->expiry_time.tv_sec &&
              th->heap[index]->expiry_time.tv_nsec < th->heap[(index-1)/2]->expiry_time.tv_nsec))) {
            sift_up(th, index);
        } else {
            sift_down(th, index);
        }
    }

    timer_node_pool_release(&th->node_pool, node_to_remove);
    conn->timer_node = NULL;
}

int timer_heap_get_next_timeout(timer_heap_t *th) {
    if (th->size == 0) return -1; // Бесконечное ожидание

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long long diff_ms = (th->heap[0]->expiry_time.tv_sec - now.tv_sec) * 1000;
    diff_ms += (th->heap[0]->expiry_time.tv_nsec - now.tv_nsec) / 1000000;

    return (diff_ms > 0) ? diff_ms : 0;
}

void timer_heap_process_expired(timer_heap_t *th) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    while (th->size > 0) {
        timer_node_t *top = th->heap[0];
        if (top->expiry_time.tv_sec > now.tv_sec ||
           (top->expiry_time.tv_sec == now.tv_sec && top->expiry_time.tv_nsec > now.tv_nsec)) {
            break; // Вершина кучи еще не истекла
        }

        // Таймер истек, закрываем соединение
        connection_t *conn = top->conn;
        if (conn->state != STATE_FREE && conn->state != STATE_CLOSING) {
            //printf("Worker: Closing connection %d due to timeout (state: %d)\n", conn->fd, conn->state);
            conn->state = STATE_CLOSING;
            close_connection_from_worker(conn);
        }
        // Удаляем из кучи (close_connection_from_worker уже это делает)
    }
}

// Реализация sift_up, sift_down, swap_nodes
static void swap_nodes(timer_heap_t *th, int a, int b) {
    timer_node_t *temp = th->heap[a];
    th->heap[a] = th->heap[b];
    th->heap[b] = temp;
    
    // Update heap indices
    th->heap[a]->heap_index = a;
    th->heap[b]->heap_index = b;
}

static void update_heap_index(timer_heap_t *th, int index) {
    if (index >= 0 && index < th->size) {
        th->heap[index]->heap_index = index;
    }
}

static void sift_up(timer_heap_t *th, int index) {
    if (index == 0) return;
    int parent_index = (index - 1) / 2;

    if (th->heap[index]->expiry_time.tv_sec < th->heap[parent_index]->expiry_time.tv_sec ||
       (th->heap[index]->expiry_time.tv_sec == th->heap[parent_index]->expiry_time.tv_sec &&
        th->heap[index]->expiry_time.tv_nsec < th->heap[parent_index]->expiry_time.tv_nsec))
    {
        swap_nodes(th, index, parent_index);
        sift_up(th, parent_index);
    }
}

static void sift_down(timer_heap_t *th, int index) {
    int left_child = 2 * index + 1;
    int right_child = 2 * index + 2;
    int smallest = index;

    if (left_child < th->size &&
       (th->heap[left_child]->expiry_time.tv_sec < th->heap[smallest]->expiry_time.tv_sec ||
       (th->heap[left_child]->expiry_time.tv_sec == th->heap[smallest]->expiry_time.tv_sec &&
        th->heap[left_child]->expiry_time.tv_nsec < th->heap[smallest]->expiry_time.tv_nsec)))
    {
        smallest = left_child;
    }

    if (right_child < th->size &&
       (th->heap[right_child]->expiry_time.tv_sec < th->heap[smallest]->expiry_time.tv_sec ||
       (th->heap[right_child]->expiry_time.tv_sec == th->heap[smallest]->expiry_time.tv_sec &&
        th->heap[right_child]->expiry_time.tv_nsec < th->heap[smallest]->expiry_time.tv_nsec)))
    {
        smallest = right_child;
    }

    if (smallest != index) {
        swap_nodes(th, index, smallest);
        sift_down(th, smallest);
    }
}
