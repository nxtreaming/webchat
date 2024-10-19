#define _POSIX_C_SOURCE 200809L  // 定义POSIX宏以启用strdup

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <libwebsockets.h>
#include <signal.h>

#define MAX_CLIENTS 100
#define INITIAL_BUFFER_SIZE 1024
#define MAX_MESSAGE_SIZE (1024 * 1024)  // 1MB

#define MESSAGE_TYPE_TEXT 0x01
#define MESSAGE_TYPE_AUDIO 0x02

struct ws_session {
    int client_id;
    struct lws *wsi;
    unsigned char *buffer;
    size_t length;
    size_t size;
    unsigned char message_type;
};

static struct ws_session *clients[MAX_CLIENTS] = {0};
static int force_exit = 0;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    force_exit = 1;
}

// Extract client_id from query string
static int extract_client_id_from_query(const char *query_string) {
    char *client_id_str = NULL;
    char *query_copy = strdup(query_string);  // Make a copy of the query string
    if (!query_copy) {
        lwsl_err("strdup failed for query_string\n");
        return -1;
    }

    char *token = strtok(query_copy, "&");
    while (token != NULL) {
        if (strncmp(token, "client-id=", 10) == 0) {
            client_id_str = token + 10;
            break;
        }
        token = strtok(NULL, "&");
    }

    int client_id = -1;
    if (client_id_str != NULL) {
        client_id = atoi(client_id_str);
        if (client_id <= 0) {
            lwsl_err("Invalid client-id value: %s\n", client_id_str);
            client_id = -1;  // Simple validation
        } else {
            lwsl_notice("Parsed client-id: %d\n", client_id);
        }
    } else {
        lwsl_err("client-id parameter not found in query string\n");
    }

    free(query_copy);
    return client_id;
}

// Find client by ID
static int find_client_by_id(int client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->client_id == client_id) {
            return i;
        }
    }
    return -1;
}

// Extend buffer with growth factor
static int extend_buffer(struct ws_session *pss, size_t additional_size) {
    size_t required_size = pss->length + additional_size;
    if (required_size > pss->size) {
        size_t new_size = pss->size * 2;
        while (new_size < required_size) {
            new_size *= 2;
        }
        unsigned char *new_buffer = realloc(pss->buffer, new_size);
        if (!new_buffer) {
            lwsl_err("Failed to extend buffer\n");
            return -1;
        }
        pss->buffer = new_buffer;
        pss->size = new_size;
    }
    return 0;
}

// WebSocket callback
static int callback_websocket(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    struct ws_session *pss = (struct ws_session *)user;

    switch (reason) {
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
            // Get the full query string
            char query_string[1024];  // Increased buffer size
            int n = lws_hdr_copy_fragment(wsi, query_string, sizeof(query_string), WSI_TOKEN_HTTP_URI_ARGS, 0);
            if (n <= 0) {
                lwsl_err("Failed to get query string\n");
                return -1;
            }

            // Extract client_id
            int client_id = extract_client_id_from_query(query_string);
            if (client_id == -1) {
                lwsl_err("Invalid or missing client-id\n");
                return -1;
            }

            // Check if client_id is unique
            if (find_client_by_id(client_id) != -1) {
                lwsl_err("Duplicate client-id: %d\n", client_id);
                return -1;
            }

            // Allow connection
            break;
        }

        case LWS_CALLBACK_ESTABLISHED: {
            // Allocate and initialize ws_session
            pss->buffer = malloc(INITIAL_BUFFER_SIZE);
            if (!pss->buffer) {
                lwsl_err("Failed to allocate initial buffer\n");
                return -1;
            }
            pss->size = INITIAL_BUFFER_SIZE;
            pss->length = 0;
            pss->message_type = 0;
            pss->wsi = wsi;

            // Assign client_id from query
            char query_string[1024];
            int n = lws_hdr_copy_fragment(wsi, query_string, sizeof(query_string), WSI_TOKEN_HTTP_URI_ARGS, 0);
            if (n <= 0) {
                lwsl_err("Failed to retrieve query string on established\n");
                free(pss->buffer);
                return -1;
            }
            pss->client_id = extract_client_id_from_query(query_string);
            if (pss->client_id == -1) {
                lwsl_err("Invalid client-id on established\n");
                free(pss->buffer);
                return -1;
            }

            // Add to clients array
            bool added = false;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!clients[i]) {
                    clients[i] = pss;
                    added = true;
                    break;
                }
            }
            if (!added) {
                lwsl_err("Max clients reached, closing connection\n");
                free(pss->buffer);
                return -1;
            }

            lwsl_notice("Client %d connected\n", pss->client_id);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            // Check message size
            if (pss->length + len > MAX_MESSAGE_SIZE) {
                lwsl_err("Message too large from client %d, closing connection\n", pss->client_id);
                return -1;
            }

            // Extend buffer if necessary
            if (extend_buffer(pss, len) < 0) {
                return -1;
            }

            // If first byte, set message_type
            if (pss->message_type == 0 && pss->length == 0) {
                pss->message_type = ((unsigned char *)in)[0];
                in = (unsigned char *)in + 1;
                len -= 1;
                if (len == 0) break;  // No data left after message_type
            }

            // Append data to buffer
            memcpy(pss->buffer + pss->length, in, len);
            pss->length += len;

            // If final fragment, process message
            if (lws_is_final_fragment(wsi)) {
                if (pss->message_type == MESSAGE_TYPE_TEXT) {
                    // Ensure null-termination
                    if (extend_buffer(pss, 1) < 0) {
                        return -1;
                    }
                    pss->buffer[pss->length] = '\0';
                    lwsl_info("Received text from client %d: %s\n", pss->client_id, pss->buffer);
                } else if (pss->message_type == MESSAGE_TYPE_AUDIO) {
                    lwsl_info("Received audio from client %d, size: %zu bytes\n", pss->client_id, pss->length);
                }

                // Broadcast to other clients
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i] && clients[i]->client_id != pss->client_id) {
                        // 动态分配发送缓冲区，预留1字节用于message_type
                        unsigned char *send_buffer = malloc(LWS_PRE + 1 + pss->length);
                        if (!send_buffer) {
                            lwsl_err("Failed to allocate send buffer for client %d\n", clients[i]->client_id);
                            continue;
                        }
                        memset(send_buffer, 0, LWS_PRE);  // 清零前缀

                        // 设置message_type为第一个字节
                        send_buffer[LWS_PRE] = pss->message_type;

                        // 复制payload到发送缓冲区
                        memcpy(send_buffer + LWS_PRE + 1, pss->buffer, pss->length);

                        // 发送消息
                        int bytes = lws_write(clients[i]->wsi, send_buffer + LWS_PRE, 1 + pss->length, 
                                              pss->message_type == MESSAGE_TYPE_TEXT ? LWS_WRITE_TEXT : LWS_WRITE_BINARY);
                        if (bytes < (int)(1 + pss->length)) {
                            lwsl_err("Failed to send message to client %d\n", clients[i]->client_id);
                        } else {
                            lwsl_debug("Sent %d bytes to client %d\n", bytes, clients[i]->client_id);
                        }

                        free(send_buffer);  // 释放发送缓冲区
                    }
                }

                // Reset buffer
                pss->length = 0;
                pss->message_type = 0;
            }

            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (pss->buffer) {
                free(pss->buffer);
                pss->buffer = NULL;
            }
            lwsl_notice("Client %d disconnected\n", pss->client_id);
            int client_index = find_client_by_id(pss->client_id);
            if (client_index != -1) {
                clients[client_index] = NULL;
            }
            break;
        }

        default:
            break;
    }

    return 0;
}

static struct lws_protocols protocols[] = {
    {"http-only", lws_callback_http_dummy, 0, 0},
    {"websocket-chat", callback_websocket, sizeof(struct ws_session), INITIAL_BUFFER_SIZE},
    {NULL, NULL, 0, 0}
};

int main() {
    struct lws_context_creation_info info;
    struct lws_context *context;

    memset(&info, 0, sizeof(info));
    info.port = 8080;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8; 

    lws_set_log_level(LLL_NOTICE | LLL_ERR | LLL_WARN, NULL);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("Failed to create context\n");
        return -1;
    }

    lwsl_notice("Starting server on port %d\n", info.port);
    while (!force_exit) {
        int n = lws_service(context, 100);
        if (n < 0) {
            lwsl_err("lws_service error\n");
            break;
        }
    }

    lws_context_destroy(context);
    lwsl_notice("Server terminated gracefully\n");
    return 0;
}

