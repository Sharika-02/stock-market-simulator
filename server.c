#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include "database.h"
#include "simulator.h"

#define PORT 8080
#define MAX_CLIENTS 100
#define STOCK_COUNT 12
int clients[MAX_CLIENTS], client_count = 0;
pthread_mutex_t lock;

/* STOCK DATA */
char *stocks[] = {
"AAPL","GOOG","TSLA","MSFT","AMZN","NVDA",
"META","NFLX","INTC","AMD","IBM","ORCL"};
float prices[] = {
100,200,300,150,250,180,
220,190,90,120,140,160};

/* CLIENT INFO */
typedef struct {
    int sock, port;
    char ip[INET_ADDRSTRLEN];
} client_info;

void websocket_handshake(int sock)
{
    char buffer[2048];
    int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
    if(bytes <= 0){ close(sock); return; }
    buffer[bytes] = '\0';
    if(!strstr(buffer,"token=stock123")){
        printf("[SECURITY] authentication failed\n");
        close(sock); return;
    }
    char *key_start = strstr(buffer,"Sec-WebSocket-Key:");
    if(!key_start){
        printf("Key not found\n");
        close(sock); return;
    }
    key_start += strlen("Sec-WebSocket-Key: ");
    char key[128], combined[256], base64[256], response[512];
    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    sscanf(key_start,"%127s",key);
    snprintf(combined,sizeof(combined),
        "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    SHA1((unsigned char*)combined, strlen(combined), sha1_hash);
    EVP_EncodeBlock((unsigned char*)base64, sha1_hash, SHA_DIGEST_LENGTH);
    sprintf(response,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", base64);
    send(sock,response,strlen(response),0);
}

int websocket_send(int client, char *msg)
{
    int len = strlen(msg), total = 0, frame_len = len + 2;
    unsigned char frame[1024];
    frame[0] = 0x81;
    frame[1] = len;
    memcpy(frame+2,msg,len);
    while(total < frame_len){
        int n = send(client,frame+total,frame_len-total,0);
        if(n <= 0) return -1;
        total += n;
    }
    return total;
}

void broadcast(char *msg)
{
    pthread_mutex_lock(&lock);
    for(int i=0;i<client_count;i++){
        if(websocket_send(clients[i],msg) < 0){
            close(clients[i]);
            clients[i] = clients[--client_count];
            i--;
        }
    }
    pthread_mutex_unlock(&lock);
}

void *stock_updates(void *arg)
{
    while(1){
        for(int i=0;i<STOCK_COUNT;i++){
            prices[i] = generate_price(prices[i]);
            char msg[128];
            sprintf(msg,"%s %.2f\n",stocks[i],prices[i]);
            printf("[BROADCAST] %s",msg);
            broadcast(msg);
            insert_price(stocks[i],prices[i]);
        }
        sleep(3);
    }
}

void *client_handler(void *arg)
{
    client_info *client = (client_info*)arg;
    char buffer[1024];
    while(recv(client->sock,buffer,sizeof(buffer),0) > 0);
    printf("[SERVER] client disconnected: %s:%d\n",
           client->ip,client->port);
    char logmsg[128];
    sprintf(logmsg,"%s:%d disconnected",client->ip,client->port);
    log_event("client",logmsg);
    close(client->sock);
    free(client);
    return NULL;
}

int main()
{
    int server_fd, opt=1, flag=1;
    struct sockaddr_in address;
    pthread_mutex_init(&lock,NULL);
    init_db();
    server_fd = socket(AF_INET,SOCK_STREAM,0);
    setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    setsockopt(server_fd,SOL_SOCKET,SO_KEEPALIVE,&opt,sizeof(opt));
    setsockopt(server_fd,IPPROTO_TCP,TCP_NODELAY,&flag,sizeof(flag));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    bind(server_fd,(struct sockaddr*)&address,sizeof(address));
    listen(server_fd,10);
    printf("Server running on port %d\n",PORT);
    pthread_t stock_thread;
    pthread_create(&stock_thread,NULL,stock_updates,NULL);
    while(1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_fd,
            (struct sockaddr*)&client_addr,&client_len);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,&client_addr.sin_addr,
                  client_ip,INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);
        printf("[SERVER] client connected: %s:%d\n",
               client_ip,client_port);
        char logmsg[128];
        sprintf(logmsg,"%s:%d connected",client_ip,client_port);
        log_event("client",logmsg);
        websocket_handshake(client_sock);
        pthread_mutex_lock(&lock);
        if(client_count < MAX_CLIENTS)
            clients[client_count++] = client_sock;
        pthread_mutex_unlock(&lock);
        client_info *client = malloc(sizeof(client_info));
        client->sock = client_sock;
        client->port = client_port;
        strcpy(client->ip,client_ip);
        pthread_t thread_id;
        pthread_create(&thread_id,NULL,client_handler,client);
    }
}