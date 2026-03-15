#pragma once

typedef void* esp_http_client_handle_t;
typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERR_NO_MEM -1

enum http_event {
    HTTP_EVENT_ON_DATA
};

struct esp_http_client_event_t {
    http_event event_id;
    esp_http_client_handle_t client;
    void* data;
    int data_len;
};

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char* key, const char* value);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
int esp_http_client_get_content_length(esp_http_client_handle_t client);