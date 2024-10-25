#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
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
    int64_t client_id;
    int64_t subscribe_id;
    int client_role;
    struct lws *wsi;
    unsigned char *buffer;
    size_t length;
    size_t size;
    unsigned char message_type;
    unsigned char *send_buffer;
    size_t send_length;
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

// Helper function to retrieve the full query string by concatenating all fragments
static int get_full_query_string(struct lws *wsi, char *query_string, size_t max_size) {
    int total_length = lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_URI_ARGS);
    if (total_length < 0) {
        lwsl_err("Failed to get total length of query string\n");
        return -1;
    }

    if ((size_t)total_length >= max_size) {
        lwsl_err("Query string too long\n");
        return -1;
    }

    int copied = lws_hdr_copy(wsi, WSI_TOKEN_HTTP_URI_ARGS, query_string, max_size);
    if (copied < 0) {
        lwsl_err("Failed to copy query string\n");
        return -1;
    }

    // Log the complete query string for verification
    lwsl_info("Full query string: %s\n", query_string);

    return 0;
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
    char *key_str = "client-role=";
    int key_len = strlen(key_str);
    while (token != NULL) {
        if (strncmp(token, key_str, key_len) == 0) {
            client_role_str = token + key_len;
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
static int64_t extract_client_id_from_query(const char *query_string) {
    char *client_id_str = NULL;
    char *query_copy = strdup(query_string);
    if (!query_copy) {
        lwsl_err("strdup failed for query_string\n");
        return -1;
    }

    char *token = strtok(query_copy, "&");
    char *key_str = "client-id=";
    int key_len = strlen(key_str);
    while (token != NULL) {
        if (strncmp(token, key_str, key_len) == 0) {
            client_id_str = token + key_len;
            break;
        }
        token = strtok(NULL, "&");
    }

    int64_t client_id = -1;
    if (client_id_str != NULL) {
        client_id = strtoll(client_id_str, NULL, 10);
        if (client_id <= 0) {
            lwsl_err("Invalid client-id value: %s\n", client_id_str);
            client_id = -1;  // Simple validation
        } else {
            lwsl_notice("Parsed client-id: %" PRIi64 "\n", client_id);
        }
    } else {
        lwsl_err("client-id parameter not found in query string\n");
    }

    free(query_copy);
    return client_id;
}

// Extract subscribe_id from query string
static int64_t extract_subscribe_id_from_query(const char *query_string) {
    char *subscribe_id_str = NULL;
    char *query_copy = strdup(query_string);
    if (!query_copy) {
        lwsl_err("strdup failed for query_string\n");
        return -1;
    }

    char *token = strtok(query_copy, "&");
    char *key_str = "subscribe-id=";
    int key_len = strlen(key_str);
    while (token != NULL) {
        if (strncmp(token, key_str, key_len) == 0) {
            subscribe_id_str = token + key_len;
            break;
        }
        token = strtok(NULL, "&");
    }

    int64_t subscribe_id = -1;
    if (subscribe_id_str != NULL) {
        subscribe_id = strtoll(subscribe_id_str, NULL, 10);
        if (subscribe_id <= 0) {
            lwsl_err("Invalid subscribe-id value: %s\n", subscribe_id_str);
            subscribe_id = -1;
        } else {
            lwsl_notice("Parsed subscribe-id: %" PRIi64 "\n", subscribe_id);
        }
    } else {
        lwsl_err("subscribe-id parameter not found in query string\n");
    }

    free(query_copy);
    return subscribe_id;
}

// Find a client by client_id
static int find_client_by_id(int64_t client_id) {
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
            lwsl_info("wedebug:Allocated buffer %d from pool\n", i);
            lwsl_info("wedebug:Increased ref_count of buffer %d to %d\n", i, pool.ref_count[i]);
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
            lwsl_info("wedebug:Increased ref_count of buffer %d to %d\n", i, pool.ref_count[i]);
            break;
        }
    }
}

// Release a buffer (decrease reference count)
static void release_buffer(unsigned char *buffer) {
    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (pool.buffers[i] == buffer) {
            pool.ref_count[i]--;
            if (pool.ref_count[i] < 0) {
                lwsl_warn("wedebug:Underflow: Decreased ref_count of buffer %d to %d\n", i, pool.ref_count[i]);
                pool.ref_count[i] = 0;
            }
            lwsl_info("wedebug:Decreased ref_count of buffer %d to %d\n", i, pool.ref_count[i]);
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
        lwsl_info("No target clients to broadcast message from client %" PRIi64 "\n", sender->client_id);
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

    // Iterate over clients and prepare to send message
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->client_id != sender->client_id && clients[i]->client_role != CLIENT_ROLE_HOST) {
            // if client has a valid host_id, it would only receive messages from its host_id
            if (clients[i]->subscribe_id > 0 && clients[i]->subscribe_id != sender->client_id)
                continue;
            // Check and release existing send_buffer
            if (clients[i]->send_buffer != NULL) {
                lwsl_warn("wedebug:client[%d]@ broadcast\n", i);
                release_buffer(clients[i]->send_buffer);
                clients[i]->send_buffer = NULL;
            }

            // Assign send_buffer to client
            clients[i]->send_buffer = send_buffer;

            // Set send_length for the client
            clients[i]->send_length = 1 + payload_len; // 1 byte for message_type + payload

            // Increase buffer reference count
            add_ref_to_buffer(send_buffer);

            // Request writeable callback to send the message in the appropriate callback
            lws_callback_on_writable(clients[i]->wsi);
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
            char query_string[4096];
            if (get_full_query_string(wsi, query_string, sizeof(query_string)) != 0) {
                lwsl_err("Failed to get full query string\n");
                return -1;
            }

            // Extract client_id
            int64_t client_id = extract_client_id_from_query(query_string);
            if (client_id == -1) {
                lwsl_err("Invalid or missing client-id\n");
                return -1;
            }

            // Check for duplicate client_id
            int existing_index = find_client_by_id(client_id);
            if (existing_index != -1) {
                struct ws_session *existing_client = clients[existing_index];
                if (existing_client && existing_client->wsi) {
                    lwsl_notice("Duplicate client-id: %" PRIi64 ", closing existing connection\n", client_id);
                    // Close the existing connection
                    lws_close_reason(existing_client->wsi, LWS_CLOSE_STATUS_NORMAL, (unsigned char *)"Duplicate client-id", strlen("Duplicate client-id"));
                    // Remove the old client from the clients array
                    clients[existing_index] = NULL;
                }
            }

            // Continue processing the new connection
            pss->subscribe_id = extract_subscribe_id_from_query(query_string);

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
            pss->send_length = 0;

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

            lwsl_notice("Client %" PRIi64 " connected with role %d\n", pss->client_id, pss->client_role);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            // Check message size
            if (pss->length + len > MAX_MESSAGE_SIZE) {
                lwsl_err("Message too large from client %" PRIi64 ", closing connection\n", pss->client_id);
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
                    lwsl_info("Received text from client %" PRIi64 ": %s\n", pss->client_id, pss->buffer);
                } else if (pss->message_type == MESSAGE_TYPE_AUDIO) {
                    lwsl_info("Received audio from client %" PRIi64 ", size: %zu bytes\n", pss->client_id, pss->length);
                }

                // Broadcast message
                broadcast_message(pss, pss->message_type, pss->buffer, pss->length);

                // Reset receive buffer
                pss->length = 0;
                pss->message_type = 0;
            }

            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            lwsl_info("wedebug:server writeable:client-id=%" PRIi64 "\n", pss->client_id);
            // Send completion callback
            if (pss->send_buffer) {
                // Determine the data length to send
                // Since we always use LWS_WRITE_BINARY, data_len is always 1 + payload_len
                size_t data_len = pss->send_length; // 1 byte for message_type + payload length

                // Call lws_write() to send the data
                int bytes = lws_write(pss->wsi, pss->send_buffer + LWS_PRE, data_len, LWS_WRITE_BINARY);
                if (bytes < (int)data_len) {
                    lwsl_err("Failed to send message to client %" PRIi64 "\n", pss->client_id);
                    // Optionally handle the error, e.g., close the connection
                } else {
                    lwsl_info("Sent %d bytes to client %" PRIi64 "\n", bytes, pss->client_id);
                }

                // Release the send buffer after sending
                release_buffer(pss->send_buffer);
                pss->send_buffer = NULL;
                pss->send_length = 0;
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
                lwsl_info("wedebug:closed\n");
                pss->send_buffer = NULL;
                pss->send_length = 0;
            }
            lwsl_notice("Client %" PRIi64 " disconnected\n", pss->client_id);

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
    info.port = 28080;
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
