#define _GNU_SOURCE
#include "worker.h"
#include "http_handler.h"
#include "timer.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <netinet/tcp.h>

#define MAX_EVENTS_PER_WORKER 1024
#define REQUEST_TIMEOUT_MS 5000
#define KEEP_ALIVE_TIMEOUT_MS 10000
#define MAX_ACCEPTS_PER_LOOP 64
#define MAX_REQUEST_SIZE 8192

// Определяем структуру для передачи данных в тред
typedef struct {
    int server_fd;
    int worker_id;
} worker_args_t;

// Глобальные переменные для каждого воркера (thread-local)
static __thread int epoll_fd;
static __thread timer_heap_t timer_heap;

// Вспомогательные функции, специфичные для воркера
static void handle_new_connection(int server_fd);
static void handle_connection_event(connection_t *conn, uint32_t events);
static void do_read(connection_t *conn);
static void do_write(connection_t *conn);

void *worker_loop(void *arg) {
    worker_args_t *args = (worker_args_t*)arg;
    int server_fd = args->server_fd;

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        return NULL;
    }

    if (timer_heap_init(&timer_heap, 65536) != 0) {
        perror("timer_heap_init");
        return NULL;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLEXCLUSIVE; // EPOLLEXCLUSIVE для `SO_REUSEPORT`
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd");
        return NULL;
    }

    printf("Worker %d started.\n", args->worker_id);

    struct epoll_event events[MAX_EVENTS_PER_WORKER];

    extern volatile sig_atomic_t g_running;
    while (g_running) {
        int timeout = timer_heap_get_next_timeout(&timer_heap);
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS_PER_WORKER, timeout);

        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        timer_heap_process_expired(&timer_heap);

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == server_fd) {
                handle_new_connection(server_fd);
            } else {
                handle_connection_event(events[i].data.ptr, events[i].events);
            }
        }
    }

    printf("Worker %d shutting down.\n", args->worker_id);
    timer_heap_destroy(&timer_heap);
    close(epoll_fd);
    return NULL;
}

static void handle_new_connection(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int accepts_count = 0;
    while (accepts_count < MAX_ACCEPTS_PER_LOOP) {
        int client_fd = accept4(server_fd, (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Нет больше входящих соединений
            perror("accept4");
            break;
        }
        accepts_count++;

        // Set TCP optimizations
        int flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            perror("setsockopt(TCP_NODELAY)");
        }
        
        // Set socket buffer sizes for better performance
        int sndbuf = 65536, rcvbuf = 65536;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        connection_t *conn = connection_get();
        if (!conn) {
            fprintf(stderr, "Connection pool exhausted. Dropping new connection.\n");
            close(client_fd);
            continue;
        }

        conn->fd = client_fd;
        conn->client_addr = client_addr;
        conn->state = STATE_READING;
        clock_gettime(CLOCK_MONOTONIC, &conn->last_active);

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = conn;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
            perror("epoll_ctl: client_fd");
            connection_release(conn);
            close(client_fd);
            continue;
        }

        timer_heap_add(&timer_heap, conn, REQUEST_TIMEOUT_MS);
    }
}

static void handle_connection_event(connection_t *conn, uint32_t events) {
    if (conn->state == STATE_CLOSING || conn->state == STATE_FREE) {
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &conn->last_active);

    if (events & (EPOLLERR | EPOLLHUP)) {
        close_connection_from_worker(conn);
        return;
    }

    if (conn->state == STATE_READING || conn->state == STATE_KEEP_ALIVE) {
        if (events & EPOLLIN) {
            do_read(conn);
        }
    }

    if (conn->state == STATE_WRITING) {
        if (events & EPOLLOUT) {
            do_write(conn);
        }
    }
}

static void do_read(connection_t *conn) {
    conn->state = STATE_READING;
    timer_heap_remove(&timer_heap, conn); // Убираем старый таймер (keep-alive)
    timer_heap_add(&timer_heap, conn, REQUEST_TIMEOUT_MS); // Ставим новый таймер на запрос

    ssize_t nread;
    int read_attempts = 0;
    const int MAX_READ_ATTEMPTS = 16; // Prevent infinite loops
    
    do {
        size_t space_left = BUFFER_SIZE - conn->bytes_read;
        if (space_left == 0) {
            // Buffer full - potential DoS attack
            close_connection_from_worker(conn);
            return;
        }
        
        nread = recv(conn->fd, conn->read_buf + conn->bytes_read, space_left, 0);
        if (nread > 0) {
            conn->bytes_read += nread;
            // Check for oversized request
            if (conn->bytes_read > MAX_REQUEST_SIZE) {
                close_connection_from_worker(conn);
                return;
            }
            
            // Check for suspicious patterns (basic DoS protection)
            if (conn->bytes_read > 1024) {
                // Look for repeated characters (potential attack)
                char *buf = conn->read_buf;
                int repeated_count = 0;
                for (size_t i = 1; i < conn->bytes_read && i < 256; i++) {
                    if (buf[i] == buf[i-1]) {
                        repeated_count++;
                        if (repeated_count > 128) { // Too many repeated chars
                            close_connection_from_worker(conn);
                            return;
                        }
                    } else {
                        repeated_count = 0;
                    }
                }
            }
        }
        
        read_attempts++;
        if (read_attempts > MAX_READ_ATTEMPTS) {
            break; // Prevent infinite reading
        }
    } while (nread > 0 && conn->bytes_read < BUFFER_SIZE);

    if (nread == 0 || (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Клиент закрыл соединение или ошибка
        close_connection_from_worker(conn);
        return;
    }

    size_t parsed = http_parser_execute(&conn->parser, &parser_settings, conn->read_buf, conn->bytes_read);

    if (conn->parser.http_errno != HPE_OK && conn->parser.http_errno != HPE_PAUSED) {
        // Ошибка парсинга
        close_connection_from_worker(conn);
        return;
    }
    
    // Additional security checks
    if (parsed != conn->bytes_read && conn->parser.http_errno != HPE_PAUSED) {
        // Not all data was parsed - potential attack
        close_connection_from_worker(conn);
        return;
    }
    
    // Check HTTP version (only allow 1.0 and 1.1)
    if (conn->parser.http_major != 1 || (conn->parser.http_minor != 0 && conn->parser.http_minor != 1)) {
        close_connection_from_worker(conn);
        return;
    }

    if (conn->parser.upgrade) {
        // Не поддерживаем Upgrade
        close_connection_from_worker(conn);
        return;
    }

    if (conn->state == STATE_READING) { // on_headers_complete мог не сработать еще
        if (conn->bytes_read >= MAX_REQUEST_SIZE) { // Запрос слишком большой
            close_connection_from_worker(conn);
            return;
        }
        // Не все данные еще пришли, перевзводим epoll
        struct epoll_event ev = { .events = EPOLLIN | EPOLLET | EPOLLONESHOT, .data.ptr = conn };
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
    } else {
        // Заголовки обработаны, переходим к записи
        timer_heap_remove(&timer_heap, conn);
        handle_request_and_prepare_response(conn);
        do_write(conn);
    }
}


static void do_write(connection_t *conn) {
    ssize_t nwritten;
    size_t total_len = conn->response_iov[0].iov_len + conn->response_iov[1].iov_len;

    // Validate response size
    if (total_len > 65536) { // 64KB max response
        close_connection_from_worker(conn);
        return;
    }

    int write_attempts = 0;
    const int MAX_WRITE_ATTEMPTS = 64; // Prevent infinite loops
    
    while (conn->bytes_sent < total_len && write_attempts < MAX_WRITE_ATTEMPTS) {
        // Adjust iovec for partial writes
        struct iovec adjusted_iov[2];
        size_t remaining = total_len - conn->bytes_sent;
        size_t offset = conn->bytes_sent;
        
        if (offset < conn->response_iov[0].iov_len) {
            // Still writing headers
            adjusted_iov[0].iov_base = (char*)conn->response_iov[0].iov_base + offset;
            adjusted_iov[0].iov_len = conn->response_iov[0].iov_len - offset;
            adjusted_iov[1] = conn->response_iov[1];
        } else {
            // Writing body
            size_t body_offset = offset - conn->response_iov[0].iov_len;
            adjusted_iov[0].iov_base = (char*)conn->response_iov[1].iov_base + body_offset;
            adjusted_iov[0].iov_len = conn->response_iov[1].iov_len - body_offset;
            adjusted_iov[1].iov_base = NULL;
            adjusted_iov[1].iov_len = 0;
        }
        
        nwritten = writev(conn->fd, adjusted_iov, adjusted_iov[1].iov_len > 0 ? 2 : 1);
        if (nwritten < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Буфер записи переполнен, ждем EPOLLOUT
                struct epoll_event ev = { .events = EPOLLOUT | EPOLLET | EPOLLONESHOT, .data.ptr = conn };
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                return;
            }
            // Другая ошибка
            close_connection_from_worker(conn);
            return;
        }
        conn->bytes_sent += nwritten;
        write_attempts++;
    }
    
    if (write_attempts >= MAX_WRITE_ATTEMPTS) {
        // Too many write attempts - potential slow client attack
        close_connection_from_worker(conn);
        return;
    }

    // Ответ отправлен полностью
    if (conn->keep_alive) {
        conn->state = STATE_KEEP_ALIVE;
        http_parser_init(&conn->parser, HTTP_REQUEST); // Готовимся к новому запросу
        conn->bytes_read = 0;
        conn->bytes_sent = 0;
        memset(conn->url, 0, sizeof(conn->url));

        struct epoll_event ev = { .events = EPOLLIN | EPOLLET | EPOLLONESHOT, .data.ptr = conn };
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        timer_heap_add(&timer_heap, conn, KEEP_ALIVE_TIMEOUT_MS);
    } else {
        close_connection_from_worker(conn);
    }
}

void close_connection_from_worker(connection_t *conn) {
    if (conn->fd == -1) return;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL) == -1) {
        //perror("epoll_ctl: del");
    }
    close(conn->fd);
    timer_heap_remove(&timer_heap, conn);
    connection_release(conn);
}
