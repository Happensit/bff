#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "connection.h"
#include "worker.h"
#include "http_handler.h"

#define PORT 8080
#define WORKER_THREADS 4 // Должно быть равно или меньше числа ядер CPU

// Глобальная переменная для плавной остановки
volatile sig_atomic_t g_running = 1;

void sig_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_running = 0;
    }
}

// Определяем структуру для передачи данных в тред
typedef struct {
    int server_fd;
    int worker_id;
} worker_args_t;

int main() {
    // Установка обработчиков сигналов
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN); // Игнорируем SIGPIPE

    // Инициализация подсистем
    if (connection_pool_init() != 0) {
        fprintf(stderr, "Failed to initialize connection pool.\n");
        return EXIT_FAILURE;
    }
    routes_init();

    // Создание и настройка слушающего сокета
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int enable = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0) {
        perror("setsockopt(SO_REUSEPORT)");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on port %d with %d workers...\n", PORT, WORKER_THREADS);

    // Запуск worker-тредов
    pthread_t workers[WORKER_THREADS];
    worker_args_t *worker_args[WORKER_THREADS];
    int created_workers = 0;

    // Инициализируем массив указателей
    for (int i = 0; i < WORKER_THREADS; ++i) {
        worker_args[i] = NULL;
    }

    for (int i = 0; i < WORKER_THREADS; ++i) {
        worker_args[i] = malloc(sizeof(worker_args_t));
        if (!worker_args[i]) {
            fprintf(stderr, "Failed to allocate memory for worker args\n");
            g_running = 0;
            break;
        }
        worker_args[i]->server_fd = server_fd;
        worker_args[i]->worker_id = i + 1;
        if (pthread_create(&workers[i], NULL, worker_loop, worker_args[i]) != 0) {
            perror("pthread_create");
            free(worker_args[i]);
            worker_args[i] = NULL;
            g_running = 0; // Останавливаем все, если не удалось создать тред
            break;
        }
        created_workers++;
    }

    if (created_workers == 0) {
        fprintf(stderr, "Failed to create any worker threads\n");
        close(server_fd);
        routes_destroy();
        connection_pool_destroy();
        return EXIT_FAILURE;
    }

    // Ожидание сигнала для завершения
    while (g_running) {
        sleep(1);
    }

    printf("\nShutting down server...\n");

    // Ожидание завершения всех тредов
    for (int i = 0; i < created_workers; ++i) {
        pthread_join(workers[i], NULL);
    }

    // Освобождение памяти для аргументов воркеров
    for (int i = 0; i < WORKER_THREADS; ++i) {
        if (worker_args[i]) {
            free(worker_args[i]);
        }
    }

    // Очистка ресурсов
    close(server_fd);
    routes_destroy();
    connection_pool_destroy();

    printf("Server shut down gracefully.\n");

    return EXIT_SUCCESS;
}
