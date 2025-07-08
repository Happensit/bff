#ifndef CONNECTION_H
#define CONNECTION_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include "http_parser.h"

#define BUFFER_SIZE 4096
#define URL_MAX_LEN 256

// Состояния конечного автомата для соединения
typedef enum {
    STATE_FREE,         // Соединение в пуле, не используется
    STATE_READING,      // Чтение HTTP запроса
    STATE_WRITING,      // Запись HTTP ответа
    STATE_KEEP_ALIVE,   // Ожидание нового запроса в keep-alive соединении
    STATE_CLOSING       // Соединение помечано для закрытия
} conn_state_t;

// Структура соединения. Выделяется один раз при старте.
typedef struct connection_s {
    int fd;
    conn_state_t state;
    struct sockaddr_in client_addr;

    http_parser parser;
    char url[URL_MAX_LEN];
    int keep_alive;

    char read_buf[BUFFER_SIZE];
    size_t bytes_read;

    struct iovec response_iov[2];
    char response_headers[512];
    size_t bytes_sent;

    // Указатель на узел в куче таймеров для быстрого удаления
    void *timer_node;
    struct timespec last_active;

} connection_t;

// Инициализация пула соединений
int connection_pool_init(void);

// Освобождение ресурсов пула
void connection_pool_destroy(void);

// Получить свободное соединение из пула
connection_t *connection_get(void);

// Вернуть соединение обратно в пул
void connection_release(connection_t *conn);

#endif // CONNECTION_H
