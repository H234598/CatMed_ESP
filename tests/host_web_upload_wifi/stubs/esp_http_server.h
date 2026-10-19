#ifndef TEST_ESP_HTTP_SERVER_H
#define TEST_ESP_HTTP_SERVER_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include "esp_err.h"

typedef struct httpd_req httpd_req_t;
typedef void *httpd_handle_t;
typedef esp_err_t (*httpd_uri_func)(httpd_req_t *req);
typedef enum { HTTP_GET, HTTP_POST, HTTP_DELETE, HTTP_PUT } httpd_method_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_func handler;
    void *user_ctx;
} httpd_uri_t;

struct httpd_req {
    int content_len;
    const unsigned char *body;
    size_t body_size;
    size_t body_offset;
    size_t fail_after;
    const char *content_type;
    const char *query;
    bool authenticated;
    bool csrf_ok;
    char response_status[48];
    char response[4096];
    size_t response_size;
    bool response_complete;
    bool connection_close;
    char response_type[64];
    char content_length[16];
};

#define HTTPD_RESP_USE_STRLEN -1
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, ssize_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *, ssize_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t);
esp_err_t httpd_query_key_value(const char *, const char *, char *, size_t);

#endif
