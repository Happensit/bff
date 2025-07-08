#ifndef WORKER_H
#define WORKER_H

#include "connection.h"

// Основная функция-цикл для worker-треда
void *worker_loop(void *arg);

// Функция для закрытия соединения из любого места в воркере
void close_connection_from_worker(connection_t *conn);

#endif // WORKER_H
