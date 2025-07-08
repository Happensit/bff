#ifndef HTTP_HANDLER_H
#define HTTP_HANDLER_H

#include "connection.h"

// Инициализация роутов
void routes_init(void);

// Уничтожение таблицы роутов
void routes_destroy(void);

// Главная функция обработки запроса и формирования ответа
void handle_request_and_prepare_response(connection_t *conn);

// Настройки http-parser
extern http_parser_settings parser_settings;

#endif // HTTP_HANDLER_H
