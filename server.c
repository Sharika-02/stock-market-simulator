#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sqlite3.h>
#include "database.h"
#include "simulator.h"

#define PORT 8080
#define MAX_CLIENTS 100
#define STOCK_COUNT 12

SSL *clients[MAX_CLIENTS];
int client_count = 0;

pthread_mutex_t lock;

/* STOCK DATA */
char *stocks[] = {
    "AAPL", "GOOG", "TSLA", "MSFT", "AMZN", "NVDA",
    "META", "NFLX", "INTC", "AMD", "IBM", "ORCL"};

float prices[] = {
    100, 200, 300, 150, 250, 180,
    220, 190, 90, 120, 140, 160};

/* CLIENT INFO */
typedef struct
{
    int sock, port;
    SSL *ssl;
    char ip[INET_ADDRSTRLEN];
} client_info;

/* SSL SETUP */
SSL_CTX *create_context()
{
    const SSL_METHOD *method = TLS_server_method();
    SSL_CTX *ctx = SSL_CTX_new(method);

    if (!ctx)
    {
        perror("SSL context error");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
    return ctx;
}

void configure_context(SSL_CTX *ctx)
{
    SSL_CTX_use_certificate_file(ctx, "cert.pem", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM);
}

void handle_history_request(SSL *ssl, char *buffer)
{
    char symbol[10];

    char *start = strstr(buffer, "symbol=");
    if (!start)
        return;

    start += 7; // skip "symbol="

    int i = 0;
    while (start[i] != ' ' && start[i] != '&' && start[i] != '\r' && start[i] != '\n' && i < 9)
    {
        symbol[i] = start[i];
        i++;
    }
    symbol[i] = '\0';

    sqlite3_stmt *stmt;
    char query[256];

    sprintf(query,
            "SELECT price FROM price_history WHERE symbol='%s' ORDER BY rowid DESC LIMIT 20;",
            symbol);

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
    {
        printf("DB prepare error\n");
        return;
    }

    char json[1024] = "[";
    int first = 1;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double price = sqlite3_column_double(stmt, 0);

        char temp[32];
        sprintf(temp, "%s%.2f", first ? "" : ",", price);

        strcat(json, temp);
        first = 0;
    }

    strcat(json, "]");
    sqlite3_finalize(stmt);

    // 🔥 IMPORTANT: proper HTTP headers
    char response[2048];
    sprintf(response,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            strlen(json), json);

    SSL_write(ssl, response, strlen(response));
}

void websocket_handshake(SSL *ssl)
{
    char buffer[2048];
    int bytes = SSL_read(ssl, buffer, sizeof(buffer) - 1);

    if (bytes <= 0)
        return;
    buffer[bytes] = '\0';

    // 🔥 Handle HTTP request first
    if (strncmp(buffer, "GET /history", 12) == 0)
    {
        handle_history_request(ssl, buffer);

        // ✅ CRITICAL FIX
        SSL_shutdown(ssl);
        SSL_free(ssl);

        return;
    }

    if (!strstr(buffer, "token=stock123"))
    {
        printf("[SECURITY] authentication failed\n");
        return;
    }

    char *key_start = strstr(buffer, "Sec-WebSocket-Key:");
    if (!key_start)
        return;

    key_start += strlen("Sec-WebSocket-Key: ");

    char key[128], combined[256], base64[256], response[512];
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];

    sscanf(key_start, "%127s", key);

    snprintf(combined, sizeof(combined),
             "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);

    SHA1((unsigned char *)combined, strlen(combined), sha1_hash);

    EVP_EncodeBlock((unsigned char *)base64, sha1_hash, SHA_DIGEST_LENGTH);

    sprintf(response,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n",
            base64);

    SSL_write(ssl, response, strlen(response));
}

/* SEND FRAME */
int websocket_send(SSL *ssl, char *msg)
{
    int len = strlen(msg);
    unsigned char frame[1024];

    frame[0] = 0x81;
    frame[1] = len;

    memcpy(frame + 2, msg, len);

    return SSL_write(ssl, frame, len + 2);
}

/* BROADCAST */
void broadcast(char *msg)
{
    pthread_mutex_lock(&lock);

    for (int i = 0; i < client_count; i++)
    {
        if (websocket_send(clients[i], msg) <= 0)
        {
            SSL_free(clients[i]);
            clients[i] = clients[--client_count];
            i--;
        }
    }

    pthread_mutex_unlock(&lock);
}

/* STOCK UPDATES */
void *stock_updates(void *arg)
{
    while (1)
    {
        for (int i = 0; i < STOCK_COUNT; i++)
        {
            prices[i] = generate_price(prices[i]);

            char msg[128];
            sprintf(msg, "%s %.2f\n", stocks[i], prices[i]);

            printf("[BROADCAST] %s", msg);

            broadcast(msg);
            insert_price(stocks[i], prices[i]);
        }
        sleep(3);
    }
}

/* CLIENT HANDLER */
void *client_handler(void *arg)
{
    client_info *client = (client_info *)arg;
    char buffer[1024];

    while (SSL_read(client->ssl, buffer, sizeof(buffer)) > 0)
        ;

    printf("[SERVER] client disconnected: %s:%d\n",
           client->ip, client->port);

    SSL_shutdown(client->ssl);
    SSL_free(client->ssl);
    close(client->sock);

    free(client);
    return NULL;
}

/* MAIN */
int main()
{
    int server_fd, opt = 1, flag = 1;
    struct sockaddr_in address;

    pthread_mutex_init(&lock, NULL);
    init_db();

    /* SSL INIT */
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    SSL_CTX *ctx = create_context();
    configure_context(ctx);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    printf("Secure WebSocket Server running on port %d\n", PORT);

    pthread_t stock_thread;
    pthread_create(&stock_thread, NULL, stock_updates, NULL);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(server_fd,
                                 (struct sockaddr *)&client_addr, &client_len);

        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, client_sock);

        if (SSL_accept(ssl) <= 0)
        {
            ERR_print_errors_fp(stderr);
            close(client_sock);
            SSL_free(ssl);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  client_ip, INET_ADDRSTRLEN);

        int client_port = ntohs(client_addr.sin_port);

        printf("[SERVER] client connected: %s:%d\n",
               client_ip, client_port);

        websocket_handshake(ssl);

        // 🔥 If SSL already closed (history request), skip everything
        if (SSL_get_shutdown(ssl) != 0)
        {
            close(client_sock);
            continue;
        }

        // ✅ Only WebSocket clients reach here
        pthread_mutex_lock(&lock);
        if (client_count < MAX_CLIENTS)
            clients[client_count++] = ssl;
        pthread_mutex_unlock(&lock);

        client_info *client = malloc(sizeof(client_info));
        client->sock = client_sock;
        client->ssl = ssl;
        client->port = client_port;
        strcpy(client->ip, client_ip);

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, client_handler, client);
    }
}