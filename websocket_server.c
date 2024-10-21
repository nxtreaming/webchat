#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <libwebsockets.h>
#include <signal.h>
#include <pthread.h>

#define MAX_CLIENTS         100
#define INITIAL_BUFFER_SIZE 1024
#define MAX_MESSAGE_SIZE    (1024 * 1024 * 2)  // 2MB

#define MESSAGE_TYPE_TEXT   0x01
#define MESSAGE_TYPE_AUDIO  0x02
#define BUFFER_POOL_SIZE    100

#define CLIENT_ROLE_HOST    0x01
#define CLIENT_ROLE_LISTEN  0x02
#define CLIENT_ROLE_NORMAL  0x04

// Structure to represent a WebSocket session
struct ws_session {
    int client_id;
    int client_role;
    struct lws *wsi;
    unsigned char *buffer;
    size_t length;
    size_t size;
    unsigned char message_type;
    unsigned char *send_buffer;  // Pointer to the current buffer_pool
};

// Structure to represent the buffer pool
struct buffer_pool {
    unsigned char *buffers[BUFFER_POOL_SIZE];
    int ref_count[BUFFER_POOL_SIZE];
} pool;

// Array to hold connected clients
static struct ws_session *clients[MAX_CLIENTS] = {0};
static int force_exit = 0;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    force_exit = 1;
}

// Extract client_role from query string
static int extract_client_role_from_query(const char *query_string) {
    char *client_role_str = NULL;
    char *query_copy = strdup(query_string);
    if (!query_copy) {
        lwsl_err("strdup failed for query_string\n");
        return -1;
    }

    char *token = strtok(query_copy, "&");
    while (token != NULL) {
        if (strncmp(token, "client-role=", 12) == 0) {
            client_role_str = token + 12;
            break;
        }
        token = strtok(NULL, "&");
    }

    int client_role = CLIENT_ROLE_NORMAL;
    if (client_role_str != NULL) {
        client_role = atoi(client_role_str);
        if (client_role <= 0) {
            lwsl_err("Invalid client-role value: %s\n", client_role_str);
            client_role = CLIENT_ROLE_NORMAL;
        } else {
            lwsl_notice("Parsed client-role: %d\n", client_role);
            if (client_role == 1)
                client_role = CLIENT_ROLE_HOST;
            else if (client_role == 2)
                client_role = CLIENT_ROLE_LISTEN;
            else if (client_role == 3) 
                client_role = CLIENT_ROLE_NORMAL;
            else {
                lwsl_err("Unsupported client-role: %d, rolling back to 'normal role'\n", client_role);
                client_role = CLIENT_ROLE_NORMAL;
            }
        }
    } else {
        lwsl_err("client-role parameter not found in query string\n");
    }

    free(query_copy);
    return client_role;
}

// Extract client_id from query string
static int extract_client_id_from_query(const char *query_string) {
    char *client_id_str = NULL;
    char *query_copy = strdup(query_string);
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

// Find a client by client_id
static int find_client_by_id(int client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->client_id == client_id) {
            return i;
        }
    }
    return -1;
}

// Initialize the buffer pool
static void init_buffer_pool() {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        pool.buffers[i] = malloc(LWS_PRE + 1 + MAX_MESSAGE_SIZE);
        if (!pool.buffers[i]) {
            lwsl_err("Failed to allocate buffer pool\n");
            exit(EXIT_FAILURE);
        }
        pool.ref_count[i] = 0;
    }
}

// Cleanup the buffer pool
static void cleanup_buffer_pool() {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        free(pool.buffers[i]);
    }
}

// Get a buffer from the pool
static unsigned char *get_buffer_from_pool() {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (pool.ref_count[i] == 0) { // Available buffer
            pool.ref_count[i] = 1;
            lwsl_info("Allocated buffer %d from pool\n", i);
            return pool.buffers[i];
        }
    }
    lwsl_err("No available buffers in pool\n");
    return NULL; // No available buffer
}

// Increase reference count of a buffer
static void add_ref_to_buffer(unsigned char *buffer) {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (pool.buffers[i] == buffer) {
            pool.ref_count[i]++;
            lwsl_info("Increased ref_count of buffer %d to %d\n", i, pool.ref_count[i]);
            break;
        }
    }
}

// Release a buffer (decrease reference count)
static void release_buffer(unsigned char *buffer) {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (pool.buffers[i] == buffer) {
            pool.ref_count[i]--;
            if (pool.ref_count[i] < 0)
                pool.ref_count[i] = 0;
            lwsl_info("Decreased ref_count of buffer %d to %d\n", i, pool.ref_count[i]);
            break;
        }
    }
}

// Broadcast message to other clients
static void broadcast_message(struct ws_session *sender, unsigned char message_type, unsigned char *payload, size_t payload_len) {
    // First, determine the number of target clients
    int target_clients = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->client_id != sender->client_id && clients[i]->client_role != CLIENT_ROLE_HOST) {
            target_clients++;
        }
    }

    if (target_clients == 0) {
        // No target clients, do not allocate buffer
        lwsl_info("No target clients to broadcast message from client %d\n", sender->client_id);
        return;
    }

    unsigned char *send_buffer = get_buffer_from_pool();
    if (!send_buffer) {
        lwsl_err("No available buffers in pool to broadcast message\n");
        return;
    }

    // Set message_type
    send_buffer[LWS_PRE] = message_type;

    // Copy payload
    memcpy(send_buffer + LWS_PRE + 1, payload, payload_len);

    // Iterate over clients and send message
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->client_id != sender->client_id && clients[i]->client_role != CLIENT_ROLE_HOST) {
            // Check and release existing send_buffer
            if (clients[i]->send_buffer != NULL) {
                release_buffer(clients[i]->send_buffer);
                clients[i]->send_buffer = NULL;
            }

            // Assign send_buffer to client
            clients[i]->send_buffer = send_buffer;

            // Always use binary type to dispatch message
            int send_type = LWS_WRITE_BINARY;

            // Increase buffer reference count
            add_ref_to_buffer(send_buffer);

            // Send message
            int bytes = lws_write(clients[i]->wsi, send_buffer + LWS_PRE, 1 + payload_len, send_type);
            if (bytes < (int)(1 + payload_len)) {
                lwsl_err("Failed to send message to client %d\n", clients[i]->client_id);
                release_buffer(send_buffer); // Release on failure
                clients[i]->send_buffer = NULL;
            } else {
                // Request writeable callback to release buffer
                lws_callback_on_writable(clients[i]->wsi);
            }
        }
    }

    // Release the initial reference after broadcasting to all clients
    release_buffer(send_buffer);
}

// WebSocket callback function
static int callback_websocket(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    struct ws_session *pss = (struct ws_session *)user;

    switch (reason) {
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
            // Get the full query string
            char query_string[1024];
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

            // Store client_id in user data
            pss->client_id = client_id;

            // Store client_role in user data
            pss->client_role = extract_client_role_from_query(query_string);

            // Allow connection
            break;
        }

        case LWS_CALLBACK_ESTABLISHED: {
            // Initialize ws_session
            pss->buffer = NULL; // Buffer will be allocated when receiving messages
            pss->size = 0;
            pss->length = 0;
            pss->message_type = 0;
            pss->wsi = wsi;
            pss->send_buffer = NULL;

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
                return -1;
            }

            lwsl_notice("Client %d connected with role %d\n", pss->client_id, pss->client_role);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            // Check message size
            if (pss->length + len > MAX_MESSAGE_SIZE) {
                lwsl_err("Message too large from client %d, closing connection\n", pss->client_id);
                return -1;
            }

            // Allocate buffer if necessary
            if (pss->buffer == NULL) {
                pss->buffer = malloc(INITIAL_BUFFER_SIZE);
                if (!pss->buffer) {
                    lwsl_err("Failed to allocate buffer\n");
                    return -1;
                }
                pss->size = INITIAL_BUFFER_SIZE;
                pss->length = 0;
                pss->message_type = 0;
            }

            // Extend buffer if necessary
            if (pss->length + len > pss->size) {
                size_t new_size = pss->size * 2;
                while (new_size < pss->length + len) {
                    new_size *= 2;
                }
                unsigned char *new_buffer = realloc(pss->buffer, new_size);
                if (!new_buffer) {
                    lwsl_err("Failed to extend buffer\n");
                    free(pss->buffer);
                    pss->buffer = NULL;
                    return -1;
                }
                pss->buffer = new_buffer;
                pss->size = new_size;
            }

            // If first byte, set message_type
            if (pss->message_type == 0 && pss->length == 0) {
                pss->message_type = ((unsigned char *)in)[0];
                if (pss->message_type != MESSAGE_TYPE_TEXT && pss->message_type != MESSAGE_TYPE_AUDIO) {
                    const char *msg = "Unsupported message type";
                    lwsl_err("%s\n", msg);
                    lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR,  (unsigned char *)msg, strlen(msg));
                    return -1;
                }
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
                    if (pss->length >= pss->size) {
                        // Extend buffer for null-termination
                        unsigned char *new_buffer = realloc(pss->buffer, pss->size + 1);
                        if (!new_buffer) {
                            lwsl_err("Failed to extend buffer for null-terminator\n");
                            free(pss->buffer);
                            pss->buffer = NULL;
                            return -1;
                        }
                        pss->buffer = new_buffer;
                        pss->size += 1;
                    }
                    pss->buffer[pss->length] = '\0';
                    lwsl_info("Received text from client %d: %s\n", pss->client_id, pss->buffer);
                } else if (pss->message_type == MESSAGE_TYPE_AUDIO) {
                    lwsl_info("Received audio from client %d, size: %zu bytes\n", pss->client_id, pss->length);
                }

                // Broadcast message
                broadcast_message(pss, pss->message_type, pss->buffer, pss->length);

                // Reset buffer
                pss->length = 0;
                pss->message_type = 0;
            }

            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            // Send completion callback
            if (pss->send_buffer) {
                release_buffer(pss->send_buffer);
                pss->send_buffer = NULL;
            }
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (pss->buffer) {
                free(pss->buffer);
                pss->buffer = NULL;
            }
            if (pss->send_buffer) {
                release_buffer(pss->send_buffer);
                pss->send_buffer = NULL;
            }
            lwsl_notice("Client %d disconnected\n", pss->client_id);

            // Remove from clients array
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

// Define WebSocket protocols
static struct lws_protocols protocols[] = {
    {"http-only", lws_callback_http_dummy, 0, 0},
    {"websocket-chat", callback_websocket, sizeof(struct ws_session), INITIAL_BUFFER_SIZE},
    {NULL, NULL, 0, 0}
};

// Main function to start the server
int main() {
    struct lws_context_creation_info info;
    struct lws_context *context;

    memset(&info, 0, sizeof(info));
    info.port = 8080;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8; 

    lws_set_log_level(LLL_NOTICE | LLL_ERR | LLL_WARN | LLL_INFO, NULL);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    init_buffer_pool();

    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("Failed to create context\n");
        cleanup_buffer_pool();
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

    cleanup_buffer_pool();

    return 0;
}
