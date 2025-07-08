#include "connection.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

// Максимальное количество одновременных соединений
#define MAX_CONNECTIONS 16384  // Reduced for better memory usage

// Пул соединений - статический массив, чтобы избежать malloc в рантайме
static connection_t connection_pool[MAX_CONNECTIONS];

// Стек свободных соединений (LIFO) - используем индексы для лучшей cache locality
static int free_connections_stack[MAX_CONNECTIONS];
static int free_connections_top = -1;

// Мьютекс для защиты стека свободных соединений
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

// Статистика пула для мониторинга
static volatile int pool_used_count = 0;
static volatile int pool_peak_usage = 0;

int connection_pool_init(void) {
    printf("Initializing connection pool for %d connections...\n", MAX_CONNECTIONS);
    free_connections_top = -1;
    pool_used_count = 0;
    pool_peak_usage = 0;
    
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        connection_pool[i].fd = -1;
        connection_pool[i].state = STATE_FREE;
        connection_pool[i].timer_node = NULL;
        // Используем индексы вместо указателей для лучшей cache locality
        free_connections_stack[++free_connections_top] = i;
    }
    return 0;
}

void connection_pool_destroy(void) {
    printf("Connection pool destroyed. Peak usage: %d/%d (%.1f%%)\n", 
           pool_peak_usage, MAX_CONNECTIONS, 
           (pool_peak_usage * 100.0) / MAX_CONNECTIONS);
}

connection_t *connection_get(void) {
    pthread_mutex_lock(&pool_mutex);

    if (free_connections_top == -1) {
        // Пул исчерпан
        pthread_mutex_unlock(&pool_mutex);
        return NULL;
    }

    int conn_index = free_connections_stack[free_connections_top--];
    connection_t *conn = &connection_pool[conn_index];
    
    // Обновляем статистику
    pool_used_count++;
    if (pool_used_count > pool_peak_usage) {
        pool_peak_usage = pool_used_count;
    }

    pthread_mutex_unlock(&pool_mutex);

    // Быстрая инициализация только необходимых полей
    conn->fd = -1;
    conn->state = STATE_READING;
    conn->keep_alive = 0;
    conn->bytes_read = 0;
    conn->bytes_sent = 0;
    conn->url[0] = '\0';  // Быстрее чем memset для строки
    conn->timer_node = NULL;
    
    // Инициализируем парсер
    http_parser_init(&conn->parser, HTTP_REQUEST);
    conn->parser.data = conn;

    return conn;
}

void connection_release(connection_t *conn) {
    if (!conn || conn->state == STATE_FREE) {
        return; // Защита от double-free
    }
    
    conn->state = STATE_FREE;
    conn->fd = -1;
    conn->timer_node = NULL;

    pthread_mutex_lock(&pool_mutex);

    // Вычисляем индекс соединения в пуле
    int conn_index = conn - connection_pool;
    
    // Проверяем валидность индекса
    if (conn_index >= 0 && conn_index < MAX_CONNECTIONS && 
        free_connections_top < MAX_CONNECTIONS - 1) {
        free_connections_stack[++free_connections_top] = conn_index;
        pool_used_count--;
    }
    // Если стек полон или индекс невалидный, это ошибка логики

    pthread_mutex_unlock(&pool_mutex);
}
