#define _GNU_SOURCE
#include "worker.h"
#include "http_handler.h"
#include "timer.h"
#include "simd_utils.h"
#include "lockfree_pool.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <sys/mman.h>

#define MAX_EVENTS_PER_WORKER 2048
#define REQUEST_TIMEOUT_MS 5000
#define KEEP_ALIVE_TIMEOUT_MS 10000
#define MAX_ACCEPTS_PER_LOOP 128
#define MAX_REQUEST_SIZE 8192
#define BATCH_SIZE 32
#define IO_URING_ENTRIES 4096

// Оптимизированная структура воркера с выравниванием по cache line
typedef struct {
    int worker_id;
    int cpu_id;
    int server_fd;
    int epoll_fd;
    
    // Lock-free пул соединений
    lockfree_pool_t *connection_pool;
    
    // Batch processing буферы
    struct epoll_event event_batch[MAX_EVENTS_PER_WORKER];
    connection_t *read_batch[BATCH_SIZE];
    connection_t *write_batch[BATCH_SIZE];
    int read_batch_size;
    int write_batch_size;
    
    // Таймеры с оптимизированной кучей
    timer_heap_t timer_heap;
    
    // Статистика производительности
    uint64_t events_processed;
    uint64_t connections_accepted;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t cache_hits;
    uint64_t cache_misses;
    
    // Padding для избежания false sharing
    char padding[64];
} __attribute__((aligned(64))) optimized_worker_t;

// Thread-local worker context
static __thread optimized_worker_t *current_worker = NULL;

// Функции для batch processing
static void process_read_batch(optimized_worker_t *worker);
static void process_write_batch(optimized_worker_t *worker);
static void flush_batches(optimized_worker_t *worker);

// Оптимизированные функции обработки событий
static void handle_new_connections_batch(optimized_worker_t *worker);
static void handle_connection_event_optimized(optimized_worker_t *worker, 
                                            connection_t *conn, uint32_t events);
static int do_read_optimized(optimized_worker_t *worker, connection_t *conn);
static int do_write_optimized(optimized_worker_t *worker, connection_t *conn);

// Функции для CPU affinity и NUMA оптимизации
static int setup_worker_affinity(optimized_worker_t *worker);
static void setup_memory_policy(void);

void *worker_loop_optimized(void *arg) {
    worker_args_t *args = (worker_args_t*)arg;
    
    // Инициализация оптимизированного воркера
    optimized_worker_t worker = {0};
    worker.worker_id = args->worker_id;
    worker.server_fd = args->server_fd;
    worker.cpu_id = args->worker_id % get_nprocs();
    current_worker = &worker;
    
    // Устанавливаем CPU affinity
    if (setup_worker_affinity(&worker) != 0) {
        fprintf(stderr, "Warning: Failed to set CPU affinity for worker %d\n", 
                worker.worker_id);
    }
    
    // Настраиваем NUMA memory policy
    setup_memory_policy();
    
    // Создаем epoll с оптимизированными флагами
    worker.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (worker.epoll_fd == -1) {
        perror("epoll_create1");
        return NULL;
    }
    
    // Инициализируем таймеры
    if (timer_heap_init(&worker.timer_heap, 16384) != 0) {
        perror("timer_heap_init");
        close(worker.epoll_fd);
        return NULL;
    }
    
    // Добавляем server socket в epoll
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLEXCLUSIVE,
        .data.fd = worker.server_fd
    };
    if (epoll_ctl(worker.epoll_fd, EPOLL_CTL_ADD, worker.server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd");
        timer_heap_destroy(&worker.timer_heap);
        close(worker.epoll_fd);
        return NULL;
    }
    
    printf("Optimized worker %d started on CPU %d\n", 
           worker.worker_id, worker.cpu_id);
    
    extern volatile sig_atomic_t g_running;
    uint64_t loop_iterations = 0;
    
    while (LIKELY(g_running)) {
        // Получаем timeout для следующего таймера
        int timeout = timer_heap_get_next_timeout(&worker.timer_heap);
        
        // Batch epoll_wait для лучшей производительности
        int n = epoll_wait(worker.epoll_fd, worker.event_batch, 
                          MAX_EVENTS_PER_WORKER, timeout);
        
        if (UNLIKELY(n == -1)) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        
        // Обрабатываем истекшие таймеры
        timer_heap_process_expired(&worker.timer_heap);
        
        // Batch processing событий
        for (int i = 0; i < n; ++i) {
            if (UNLIKELY(worker.event_batch[i].data.fd == worker.server_fd)) {
                handle_new_connections_batch(&worker);
            } else {
                connection_t *conn = (connection_t*)worker.event_batch[i].data.ptr;
                
                // Prefetch следующего соединения для лучшей cache locality
                if (LIKELY(i + 1 < n && worker.event_batch[i + 1].data.ptr)) {
                    PREFETCH_READ(worker.event_batch[i + 1].data.ptr);
                }
                
                handle_connection_event_optimized(&worker, conn, 
                                                worker.event_batch[i].events);
            }
        }
        
        // Обрабатываем накопленные batch'и
        if (UNLIKELY(worker.read_batch_size > 0 || worker.write_batch_size > 0)) {
            flush_batches(&worker);
        }
        
        worker.events_processed += n;
        loop_iterations++;
        
        // Периодически выводим статистику (каждые 100K итераций)
        if (UNLIKELY((loop_iterations & 0xFFFF) == 0)) {
            printf("Worker %d: %lu events, %lu connections, %lu KB read, %lu KB written\n",
                   worker.worker_id, worker.events_processed, 
                   worker.connections_accepted,
                   worker.bytes_read / 1024, worker.bytes_written / 1024);
        }
    }
    
    printf("Optimized worker %d shutting down. Stats: %lu events processed\n",
           worker.worker_id, worker.events_processed);
    
    // Cleanup
    flush_batches(&worker);
    timer_heap_destroy(&worker.timer_heap);
    close(worker.epoll_fd);
    
    return NULL;
}

static void handle_new_connections_batch(optimized_worker_t *worker) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int accepts_count = 0;
    
    // Batch accept для лучшей производительности
    while (accepts_count < MAX_ACCEPTS_PER_LOOP) {
        int client_fd = accept4(worker->server_fd, 
                               (struct sockaddr *)&client_addr, 
                               &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        
        if (UNLIKELY(client_fd == -1)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno != EINTR) perror("accept4");
            break;
        }
        
        accepts_count++;
        worker->connections_accepted++;
        
        // TCP оптимизации
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        // Оптимальные размеры буферов
        int sndbuf = 65536, rcvbuf = 32768;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        
        // Получаем соединение из lock-free пула
        connection_t *conn = lockfree_pool_get(worker->connection_pool);
        if (UNLIKELY(!conn)) {
            fprintf(stderr, "Connection pool exhausted\n");
            close(client_fd);
            continue;
        }
        
        // Быстрая инициализация соединения
        conn->fd = client_fd;
        conn->client_addr = client_addr;
        conn->state = STATE_READING;
        clock_gettime(CLOCK_MONOTONIC, &conn->last_active);
        
        // Prefetch connection data для лучшей производительности
        prefetch_connection(conn);
        
        // Добавляем в epoll
        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP,
            .data.ptr = conn
        };
        
        if (UNLIKELY(epoll_ctl(worker->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1)) {
            perror("epoll_ctl: client_fd");
            lockfree_pool_release(worker->connection_pool, conn);
            close(client_fd);
            continue;
        }
        
        // Добавляем таймер
        timer_heap_add(&worker->timer_heap, conn, REQUEST_TIMEOUT_MS);
    }
}

static void handle_connection_event_optimized(optimized_worker_t *worker,
                                            connection_t *conn, uint32_t events) {
    if (UNLIKELY(conn->state == STATE_CLOSING || conn->state == STATE_FREE)) {
        return;
    }
    
    // Обновляем время последней активности
    clock_gettime(CLOCK_MONOTONIC, &conn->last_active);
    
    // Обрабатываем ошибки и отключения
    if (UNLIKELY(events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
        close_connection_from_worker_optimized(worker, conn);
        return;
    }
    
    // Batch processing для чтения
    if ((conn->state == STATE_READING || conn->state == STATE_KEEP_ALIVE) && 
        (events & EPOLLIN)) {
        
        if (LIKELY(worker->read_batch_size < BATCH_SIZE)) {
            worker->read_batch[worker->read_batch_size++] = conn;
        } else {
            // Batch полон, обрабатываем немедленно
            if (do_read_optimized(worker, conn) == 0) {
                // Переходим к записи
                if (LIKELY(worker->write_batch_size < BATCH_SIZE)) {
                    worker->write_batch[worker->write_batch_size++] = conn;
                } else {
                    do_write_optimized(worker, conn);
                }
            }
        }
    }
    
    // Batch processing для записи
    if (conn->state == STATE_WRITING && (events & EPOLLOUT)) {
        if (LIKELY(worker->write_batch_size < BATCH_SIZE)) {
            worker->write_batch[worker->write_batch_size++] = conn;
        } else {
            do_write_optimized(worker, conn);
        }
    }
}

static void process_read_batch(optimized_worker_t *worker) {
    for (int i = 0; i < worker->read_batch_size; i++) {
        connection_t *conn = worker->read_batch[i];
        
        // Prefetch следующего соединения
        if (LIKELY(i + 1 < worker->read_batch_size)) {
            PREFETCH_READ(worker->read_batch[i + 1]);
        }
        
        if (do_read_optimized(worker, conn) == 0) {
            // Успешно прочитали, добавляем в write batch
            if (LIKELY(worker->write_batch_size < BATCH_SIZE)) {
                worker->write_batch[worker->write_batch_size++] = conn;
            } else {
                do_write_optimized(worker, conn);
            }
        }
    }
    worker->read_batch_size = 0;
}

static void process_write_batch(optimized_worker_t *worker) {
    for (int i = 0; i < worker->write_batch_size; i++) {
        connection_t *conn = worker->write_batch[i];
        
        // Prefetch следующего соединения
        if (LIKELY(i + 1 < worker->write_batch_size)) {
            PREFETCH_READ(worker->write_batch[i + 1]);
        }
        
        do_write_optimized(worker, conn);
    }
    worker->write_batch_size = 0;
}

static void flush_batches(optimized_worker_t *worker) {
    if (worker->read_batch_size > 0) {
        process_read_batch(worker);
    }
    if (worker->write_batch_size > 0) {
        process_write_batch(worker);
    }
}

static int do_read_optimized(optimized_worker_t *worker, connection_t *conn) {
    conn->state = STATE_READING;
    timer_heap_remove(&worker->timer_heap, conn);
    timer_heap_add(&worker->timer_heap, conn, REQUEST_TIMEOUT_MS);
    
    ssize_t nread;
    int read_attempts = 0;
    const int MAX_READ_ATTEMPTS = 8;
    
    do {
        size_t space_left = BUFFER_SIZE - conn->bytes_read;
        if (UNLIKELY(space_left == 0)) {
            close_connection_from_worker_optimized(worker, conn);
            return -1;
        }
        
        nread = recv(conn->fd, conn->read_buf + conn->bytes_read, space_left, 0);
        if (LIKELY(nread > 0)) {
            conn->bytes_read += nread;
            worker->bytes_read += nread;
            
            if (UNLIKELY(conn->bytes_read > MAX_REQUEST_SIZE)) {
                close_connection_from_worker_optimized(worker, conn);
                return -1;
            }
            
            // SIMD валидация входных данных
            if (UNLIKELY(!simd_validate_url_chars(conn->read_buf + conn->bytes_read - nread, nread))) {
                close_connection_from_worker_optimized(worker, conn);
                return -1;
            }
        }
        
        read_attempts++;
    } while (LIKELY(nread > 0 && conn->bytes_read < BUFFER_SIZE && read_attempts < MAX_READ_ATTEMPTS));
    
    if (UNLIKELY(nread == 0 || (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK))) {
        close_connection_from_worker_optimized(worker, conn);
        return -1;
    }
    
    // Быстрый поиск конца заголовков с SIMD
    const char *header_end = simd_find_header_end(conn->read_buf, conn->bytes_read);
    if (header_end) {
        // Заголовки получены полностью, парсим
        size_t header_len = header_end - conn->read_buf + 4;
        size_t parsed = http_parser_execute(&conn->parser, &parser_settings, 
                                          conn->read_buf, header_len);
        
        if (UNLIKELY(conn->parser.http_errno != HPE_OK && conn->parser.http_errno != HPE_PAUSED)) {
            close_connection_from_worker_optimized(worker, conn);
            return -1;
        }
        
        // Переходим к обработке запроса
        timer_heap_remove(&worker->timer_heap, conn);
        handle_request_and_prepare_response(conn);
        return 0; // Готов к записи
    }
    
    // Заголовки еще не полные, продолжаем чтение
    struct epoll_event ev = { 
        .events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, 
        .data.ptr = conn 
    };
    epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
    return 1; // Продолжаем чтение
}

static int do_write_optimized(optimized_worker_t *worker, connection_t *conn) {
    ssize_t nwritten;
    size_t total_len = conn->response_iov[0].iov_len + conn->response_iov[1].iov_len;
    
    if (UNLIKELY(total_len > 65536)) {
        close_connection_from_worker_optimized(worker, conn);
        return -1;
    }
    
    int write_attempts = 0;
    const int MAX_WRITE_ATTEMPTS = 16;
    
    while (LIKELY(conn->bytes_sent < total_len && write_attempts < MAX_WRITE_ATTEMPTS)) {
        // Оптимизированная настройка iovec для частичной записи
        struct iovec iov[2];
        int iov_count = 0;
        size_t remaining = total_len - conn->bytes_sent;
        
        if (conn->bytes_sent < conn->response_iov[0].iov_len) {
            // Записываем заголовки
            iov[0].iov_base = (char*)conn->response_iov[0].iov_base + conn->bytes_sent;
            iov[0].iov_len = conn->response_iov[0].iov_len - conn->bytes_sent;
            iov_count++;
            
            if (remaining > iov[0].iov_len) {
                // Добавляем тело ответа
                iov[1] = conn->response_iov[1];
                iov_count++;
            }
        } else {
            // Записываем только тело
            size_t body_offset = conn->bytes_sent - conn->response_iov[0].iov_len;
            iov[0].iov_base = (char*)conn->response_iov[1].iov_base + body_offset;
            iov[0].iov_len = conn->response_iov[1].iov_len - body_offset;
            iov_count = 1;
        }
        
        nwritten = writev(conn->fd, iov, iov_count);
        if (UNLIKELY(nwritten < 0)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct epoll_event ev = { 
                    .events = EPOLLOUT | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, 
                    .data.ptr = conn 
                };
                epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                return 1; // Будем ждать EPOLLOUT
            }
            close_connection_from_worker_optimized(worker, conn);
            return -1;
        }
        
        conn->bytes_sent += nwritten;
        worker->bytes_written += nwritten;
        write_attempts++;
    }
    
    if (UNLIKELY(write_attempts >= MAX_WRITE_ATTEMPTS)) {
        close_connection_from_worker_optimized(worker, conn);
        return -1;
    }
    
    // Ответ отправлен полностью
    if (LIKELY(conn->keep_alive)) {
        // Подготавливаем к новому запросу
        conn->state = STATE_KEEP_ALIVE;
        http_parser_init(&conn->parser, HTTP_REQUEST);
        conn->parser.data = conn;
        conn->bytes_read = 0;
        conn->bytes_sent = 0;
        conn->url[0] = '\0';
        
        struct epoll_event ev = { 
            .events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, 
            .data.ptr = conn 
        };
        epoll_ctl(worker->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        timer_heap_add(&worker->timer_heap, conn, KEEP_ALIVE_TIMEOUT_MS);
    } else {
        close_connection_from_worker_optimized(worker, conn);
    }
    
    return 0;
}

void close_connection_from_worker_optimized(optimized_worker_t *worker, connection_t *conn) {
    if (UNLIKELY(conn->fd == -1)) return;
    
    epoll_ctl(worker->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    close(conn->fd);
    timer_heap_remove(&worker->timer_heap, conn);
    lockfree_pool_release(worker->connection_pool, conn);
}

static int setup_worker_affinity(optimized_worker_t *worker) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(worker->cpu_id, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
        return -1;
    }
    
    // Устанавливаем высокий приоритет для воркера
    struct sched_param param = { .sched_priority = 10 };
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        // Fallback на обычный приоритет
        if (setpriority(PRIO_PROCESS, 0, -10) == -1) {
            perror("setpriority");
        }
    }
    
    return 0;
}

static void setup_memory_policy(void) {
    // Настраиваем NUMA policy для локального доступа к памяти
#ifdef __linux__
    if (madvise(NULL, 0, MADV_HUGEPAGE) == -1) {
        // Huge pages недоступны, продолжаем без них
    }
#endif
}