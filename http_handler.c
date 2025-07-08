#include "http_handler.h"
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef struct {
    char *json_buf;    // Указатель на содержимое JSON
    size_t json_len;   // Длина содержимого
} json_value_t;

// Prometheus-метрики (заглушки, чтобы избежать C++ зависимостей)
void metric_total_requests_inc(const char* url) { /* TODO: Prometheus integration */ (void)url; }
void metric_error_requests_inc(const char* url, int code) { /* TODO: Prometheus integration */ (void)url; (void)code; }
void metric_request_latency_observe(const char* url, double latency) { /* TODO: Prometheus integration */ (void)url; (void)latency; }

// Security validation functions
static int is_valid_url_char(char c) {
    return isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '?' || c == '=' || c == '&';
}

static int validate_url(const char* url, size_t len) {
    if (len == 0 || len >= URL_MAX_LEN) return 0;
    if (url[0] != '/') return 0; // Must start with /

    for (size_t i = 0; i < len; i++) {
        if (!is_valid_url_char(url[i])) return 0;
    }

    // Check for path traversal attempts
    if (strstr(url, "..") != NULL) return 0;
    if (strstr(url, "//") != NULL) return 0;

    return 1;
}

json_value_t* load_json_value(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    struct stat st;
    if (stat(filename, &st) != 0) { fclose(f); return NULL; }
    char *buf = malloc(st.st_size + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, st.st_size, f) != st.st_size) {
        fclose(f); free(buf); return NULL;
    }
    buf[st.st_size] = '\0';
    fclose(f);

    json_value_t *val = malloc(sizeof(json_value_t));
    if (!val) { free(buf); return NULL; }
    val->json_buf = buf;
    val->json_len = st.st_size;
    return val;
}

// Таблица роутов (используем GHashTable для простоты и эффективности)
static GHashTable *routes = NULL;

// Статические ответы (zero-copy)
static const char *bonuses_json = "{\"bonuses\":[10,20,30]}";
static const char *settings_json = "{\"settings\":{\"theme\":\"dark\"}}";
static const char *games_json = "{\"games\":[\"chess\",\"poker\"]}";
static const char *health_json = "{\"status\":\"OK\"}";
static const char *not_found_json = "{\"error\":\"Not Found\"}";
static const char *bad_request_json = "{\"error\":\"Bad Request\"}";
static const char *method_not_allowed_json = "{\"error\":\"Method Not Allowed\"}";

// Callback-функции для http-parser
static int on_url_callback(http_parser* p, const char* at, size_t length) {
    connection_t* conn = (connection_t*)p->data;

    // Validate URL length and content
    if (length >= URL_MAX_LEN) {
        return -1; // URL too long
    }

    if (!validate_url(at, length)) {
        return -1; // Invalid URL format
    }

    // Safely copy URL
    size_t url_len = (length < URL_MAX_LEN - 1) ? length : URL_MAX_LEN - 1;
    memcpy(conn->url, at, url_len);
    conn->url[url_len] = '\0';
    return 0;
}

static int on_headers_complete_callback(http_parser* p) {
    connection_t* conn = (connection_t*)p->data;

    // Security checks
    if (p->content_length > 0) {
        return -1; // We don't accept request bodies
    }

    // Check for suspicious header count (potential header injection)
    if (p->nread > 8192) { // Headers too large
        return -1;
    }

    if (http_should_keep_alive(p)) {
        conn->keep_alive = 1;
    } else {
        conn->keep_alive = 0;
    }
    // Сообщаем парсеру остановиться после заголовков, т.к. тело запроса нам не нужно
    return 1;
}

void routes_init(void) {
    if (routes != NULL) {
        return; // Already initialized
    }

    routes = g_hash_table_new(g_str_hash, g_str_equal);
    if (routes == NULL) {
        return; // Failed to create hash table
    }

    g_hash_table_insert(routes, (gpointer)"/bonuses", (gpointer)bonuses_json);
    g_hash_table_insert(routes, (gpointer)"/settings", (gpointer)settings_json);
    g_hash_table_insert(routes, (gpointer)"/games", (gpointer)games_json);
    g_hash_table_insert(routes, (gpointer)"/health", (gpointer)health_json);
}

void routes_destroy(void) {
    if (routes) {
        g_hash_table_destroy(routes);
    }
}

void handle_request_and_prepare_response(connection_t *conn) {
    const char *response_body = NULL;
    int status_code = 200;
    const char *status_text = "OK";

    // Create a copy of URL for safe manipulation
    char url_copy[URL_MAX_LEN];
    strncpy(url_copy, conn->url, URL_MAX_LEN - 1);
    url_copy[URL_MAX_LEN - 1] = '\0';

    // Обрезаем query-параметры из URL для роутинга
    char *q_mark = strchr(url_copy, '?');
    if (q_mark) {
        *q_mark = '\0';
    }

    // Additional URL validation after query removal
    if (strlen(url_copy) == 0 || url_copy[0] != '/') {
        status_code = 400;
        status_text = "Bad Request";
        response_body = bad_request_json;
        conn->keep_alive = 0;
        goto prepare_response;
    }

    // Проверка метода
    if (conn->parser.method != HTTP_GET) {
        status_code = 405;
        status_text = "Method Not Allowed";
        response_body = method_not_allowed_json;
        conn->keep_alive = 0; // Закрываем соединение при ошибке клиента
    } else {
        response_body = g_hash_table_lookup(routes, url_copy);
        if (!response_body) {
            status_code = 404;
            status_text = "Not Found";
            response_body = not_found_json;
            conn->keep_alive = 0;
        }
    }

prepare_response:

    // Обновление метрик
    metric_total_requests_inc(url_copy);
    if (status_code != 200) {
        metric_error_requests_inc(url_copy, status_code);
    }

    // Формирование заголовков ответа
    char keep_alive_hdr[64] = "";
    if (conn->keep_alive) {
        snprintf(keep_alive_hdr, sizeof(keep_alive_hdr), "Connection: keep-alive\r\nKeep-Alive: timeout=%d\r\n", 10);
    } else {
        strcpy(keep_alive_hdr, "Connection: close\r\n");
    }

    size_t response_body_len = strlen(response_body);
    size_t header_len = snprintf(conn->response_headers, sizeof(conn->response_headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Server: BFF/1.0\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "%s"
        "\r\n",
        status_code, status_text, response_body_len, keep_alive_hdr);

    // Validate header length
    if (header_len >= sizeof(conn->response_headers)) {
        // Headers too large - this should not happen with our static responses
        status_code = 500;
        response_body = "{\"error\":\"Internal Server Error\"}";
        conn->keep_alive = 0;
        header_len = snprintf(conn->response_headers, sizeof(conn->response_headers),
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            strlen(response_body));
    }

    // Подготовка iovec для zero-copy отправки через writev
    conn->response_iov[0].iov_base = conn->response_headers;
    conn->response_iov[0].iov_len = header_len;
    conn->response_iov[1].iov_base = (void*)response_body;
    conn->response_iov[1].iov_len = strlen(response_body);

    conn->bytes_sent = 0;
    conn->state = STATE_WRITING; // Переводим FSM в состояние записи
}
