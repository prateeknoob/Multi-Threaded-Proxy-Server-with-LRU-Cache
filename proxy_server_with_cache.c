#include "proxy_parse.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <semaphore.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_CLIENTS 10
typedef struct cache_element cache_element;
#define MAX_BYTES 4096
#define MAX_ELEMENT_SIZE 10 * (1 << 10)
#define MAX_SIZE 200 * (1 << 20)

struct cache_element
{
    char *data;
    int len;
    char *url;
    time_t lru_time_track;
    cache_element *next;
};

cache_element *find(char *url);
int add_cache_element(char *data, int size, char *url);
void remove_cache_element();

int port_number = 8081;
int proxy_socket_id;

pthread_t tid[MAX_CLIENTS];
sem_t semaphore;
pthread_mutex_t lock;

cache_element *head;
int cache_size;

/*


**CREATES A TCP CONNECTION FROM MY PROXY TO REAL WEB SERVER**


*/
int connectRemoteServer(char *host_addr, int port_num) // open tcp connection to the actual destination server
{
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0); // create IPv4 TCP socket to talk to remote server

    if (remoteSocket < 0)
    {
        perror("Failed to create socket to connect to remote server\n");
        return -1;
    }

    struct hostent *host = gethostbyname(host_addr); // gethostbyname converts hostname to IP address

    if (host == NULL)
    {
        fprintf(stderr, "No such hostname exist\n");
        return -1;
    }

    struct sockaddr_in server_addr; // store server address details

    bzero((char *)&server_addr, sizeof(server_addr)); // clear server_addr memory
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num); // sets destination port number

    bcopy((char *)host->h_addr, (char *)&server_addr.sin_addr.s_addr, host->h_length); /* copy IP address obtained from DNS into the
                                                                    socket address structure so the proxy knows which server to connect to*/

    if (connect(remoteSocket, (struct sockaddr *)&server_addr, (size_t)sizeof(server_addr)) < 0) // initiate TCP handshake with server
    {
        fprintf(stderr, "Connection to remote server failed\n");
        return -1;
    }
    return remoteSocket; // proxy connected to real server successfully
}

/*


**HANDLE REQUEST FROM CLIENT TO SERVER AND REPLY BACK**

Takes the client’s web request, sends it to the real website,
gets the website’s reply, sends that reply back to the client, and saves it so it can be reused later.

*/

int handle_request(int clientSocketId, ParsedRequest *request, char *tempReq) // forward client and server response and cache it
{
    char *buf = (char *)malloc(sizeof(char) * MAX_BYTES); // buffer to store reconstructed http request to send to server

    // build HTTP request line, something like-> GET /index.html HTTP/1.1\r\n
    strcpy(buf, "GET ");
    strcat(buf, request->path);
    strcat(buf, " ");
    strcat(buf, request->version);
    strcat(buf, "\r\n");

    size_t len = strlen(buf);

    if (ParsedHeader_set(request, "Connection", "close") < 0) // detects failure when adding connection:close
    {
        printf("Set header key is not working\n");
    }

    if (ParsedHeader_get(request, "Host") == NULL) // ensures host file exists like-> Host: example.com
    {
        if (ParsedHeader_set(request, "Host", request->host) < 0) // if not then add it and check
        {
            printf("Set Host header key is not working\n");
        }
    }

    // unparse headers as text and add to buf, check if it is successful
    if (ParsedRequest_unparse_headers(request, buf + len, // starts writing headers from the end of request line
                                      MAX_BYTES - len     // tell how much space is left in the buffer
                                      ) < 0)
    {
        printf("Unparsing headers failed\n");
    }

    // determine remote server port
    int server_port = 80;
    if (request->port) // if specified then use it, otherwise default to 80
    {
        server_port = atoi(request->port);
    }

    int remoteSocketId = connectRemoteServer(request->host, server_port); // connect to the remote server
    if (remoteSocketId < 0)
    {
        return -1;
    }

    int bytes_sent = send(remoteSocketId, buf, strlen(buf), 0); // send request to server
    bzero(buf, MAX_BYTES);

    bytes_sent = recv(remoteSocketId, buf, MAX_BYTES - 1, 0); // receive server response

    // creates a temporary buffer to store the entire response from the server, which will be added to the cache later
    char *temp_buffer = (char *)malloc(sizeof(char) * MAX_BYTES);
    int temp_buffer_size = MAX_BYTES;
    int temp_buffer_index = 0;

    // keep sending data to client until server has no more data to send
    while (bytes_sent > 0) // initially bytes_sent contains the result of the first recv
    {
        bytes_sent = send(clientSocketId, buf, bytes_sent, 0); // send server data to the client

        for (int i = 0; i < bytes_sent; i++) // copy the same data into cache buffer
        {
            temp_buffer[temp_buffer_index] = buf[i];
            temp_buffer_index++;
        }

        // increases the allocated memory for cache storage as more data is received from the server
        temp_buffer_size += MAX_BYTES;
        temp_buffer = (char *)realloc(temp_buffer, temp_buffer_size);

        if (bytes_sent < 0)
        {
            perror("Error in sending data to client\n");
            break;
        }

        bzero(buf, MAX_BYTES);                                    // clear the receive buffer
        bytes_sent = recv(remoteSocketId, buf, MAX_BYTES - 1, 0); // receive next chunk from server
    }

    temp_buffer[temp_buffer_index] = '\0'; // adds a string terminator at the end of the reponse
                                           // why? temp_buffer is filled byte-by-byte and fxn like str_len() needs this termination

    free(buf);
    add_cache_element(temp_buffer, strlen(temp_buffer), tempReq); // add the server response to cache
    free(temp_buffer);
    close(remoteSocketId); // terminate tcp connection between proxy and server
    return 0;
}

/*


CHECK IF HTTP VERSION IS SUPPORTED BY THE PROXY


*/

int checkHTTPversion(char *msg) // check if the HTTP version in the request is supported by the proxy
{
    int version = -1;
    if (strncmp(msg, "HTTP/1.0", 8) == 0)
    {
        version = 1;
    }
    else if (strncmp(msg, "HTTP/1.1", 8) == 0)
    {
        version = 1;
    }
    else
    {
        version = -1;
    }
    return version;
}

/*

SEND ERROR MESSAGE TO CLIENT IN CASE OF INVALID REQUEST OR SERVER FAILURE


*/
int sendErrorMessage(int socket, int status_code)
{
    char str[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    switch (status_code)
    {
    case 400:
        snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>", currentTime);
        printf("400 Bad Request\n");
        send(socket, str, strlen(str), 0);
        break;

    case 403:
        snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
        printf("403 Forbidden\n");
        send(socket, str, strlen(str), 0);
        break;

    case 404:
        snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
        printf("404 Not Found\n");
        send(socket, str, strlen(str), 0);
        break;

    case 500:
        snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
        // printf("500 Internal Server Error\n");
        send(socket, str, strlen(str), 0);
        break;

    case 501:
        snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
        printf("501 Not Implemented\n");
        send(socket, str, strlen(str), 0);
        break;

    case 505:
        snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
        printf("505 HTTP Version Not Supported\n");
        send(socket, str, strlen(str), 0);
        break;

    default:
        return -1;
    }
    return 1;
}

/*


**THREAD FUNCTION TO HANDLE CLIENT REQUESTS**


*/

void *thread_fn(void *socketnew) // universal socket
{
    sem_wait(&semaphore); // decreases semaphore value,if max clients then wait

    int p;
    sem_getvalue(&semaphore, &p); // get current semaphore value
    // printf("semaphore value is: %d\n", p);

    int *t = (int *)socketnew;
    int socket = *t; // dereference to get actual socket value

    int bytes_send_client, len; // bytes received from client and its length

    char *buffer = (char *)calloc(MAX_BYTES, sizeof(char)); // memory to store data from client
    bzero(buffer, MAX_BYTES);

    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0); // receive data from client

    // tcp connection may send data in segments, so keep receiving until the end of the header
    while (bytes_send_client > 0)
    {
        len = strlen(buffer);                   // get the length of the data received so far
        if (strstr(buffer, "\r\n\r\n") == NULL) // header ends with empty line, so check if it is received
        {
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0); // read more data and append,dont overwrite
        }
        else
        {
            break;
        }
    }

    // Copies the request and checks the cache, checks cache hit and miss, if client disconnected then clean up

    // Part 1: copy request
    char *tempReq = (char *)malloc(strlen(buffer) * sizeof(char) + 1); // copy request, bcoz find function modifies it
    for (size_t i = 0; i < strlen(buffer); i++)

    {
        tempReq[i] = buffer[i];
    }
    tempReq[len] = '\0';

    // Part 2:cache lookup
    struct cache_element *temp = find(tempReq);

    // Part 3:cache hit
    if (temp != NULL) // http request was processed before
    {
        int size = temp->len / sizeof(char); // determine response size coz data is stored as char

        int pos = 0;              // tracks how many bytes have already been sent
        char response[MAX_BYTES]; // cannot send all data at once so send in chunks

        while (pos < size)
        {
            bzero(response, MAX_BYTES);
            for (int i = 0; i < MAX_BYTES; i++)
            {
                response[i] = temp->data[pos]; // copy data to response buffer
                pos++;
            }
            send(socket, response, MAX_BYTES, 0); // send response to client
        }

        printf("Data retrieved from the cache\n");
        printf("CACHE HIT\n");
    }

    // Part 4: cache miss
    // cache miss->understand request->fetch from server->reply to client
    else if (bytes_send_client > 0)
    {
        len = strlen(buffer);
        ParsedRequest *request = ParsedRequest_create(); // http request details stored here,contains method,host,path,version

        if (ParsedRequest_parse(request, buffer, len) < 0) // reads buffer and fills request structure
        {
            printf("Parsing Failed\n");
        }
        else
        {
            bzero(buffer, MAX_BYTES);

            if (!strcmp(request->method, "GET")) // proxy only supports GET method
            {
                if (request->host && request->path && checkHTTPversion(request->version) == 1) // check if all details are present
                {
                    // fetch from server and reply to client
                    bytes_send_client = handle_request(socket, request, buffer); /*handle_request() opens connection to the target server,
                                                                                sends reconstructed http request,
                                                                                receives server response,
                                                                                send response back to the client,
                                                                                store response in cache*/

                    if (bytes_send_client == -1) // handle server side failures
                    {
                        sendErrorMessage(socket, 500);
                    }
                }
                else
                {
                    sendErrorMessage(socket, 500); // invalid request details
                }
            }
            else
            {
                printf("This code doesn't support any method apart from GET\n");
            }
        }
        ParsedRequest_destroy(request); // free memory allocated for request structure
    }
    else if (bytes_send_client == 0)
    {
        printf("Client disconnected\n");
    }

    shutdown(socket, SHUT_RDWR); // SHUT_RDWR->disables further send and receive operations
    close(socket);
    free(buffer);
    sem_post(&semaphore);
    sem_getvalue(&semaphore, &p);
    // printf("Semaphore post value is: %d\n", p);
    free(tempReq);
    return NULL;
}

/*

**MAIN FUNCTION**


*/

int main(int argc, char *argv[])
{
    int client_socketid, client_len;
    struct sockaddr_in server_addr, client_addr; // sockaddr is used to store IPv4 socket address
    sem_init(&semaphore, 0, MAX_CLIENTS);        // helps prevent server overload
    pthread_mutex_init(&lock, NULL);             // ensures only one thread accesses at a time

    if (argc == 2) // checks the no of arguments
    {
        port_number = atoi(argv[1]); //./proxy->0, 8080->1
    }
    else
    {
        printf("Too few arguments\n");
        exit(1);
    }

    printf("Starting Proxy Server on port %d\n", port_number);

    // Creating a socket so a proxy can listen on a port
    proxy_socket_id = socket(AF_INET, SOCK_STREAM, 0); // AF_INET->IPv4, SOCK_STREAM->TCP connection
    if (proxy_socket_id < 0)                           // check if socket creation was successful
    {
        perror("Failed to create a socket\n");
        exit(1);
    }

    int reuse = 1; // enable address reuse

    // allows reusing of the same port
    if (setsockopt(
            proxy_socket_id,
            SOL_SOCKET,
            SO_REUSEADDR,         // allow port to be reused
            (const char *)&reuse, // setsockopt expects address of value so cast is needed
            sizeof(reuse)) < 0)
    {
        perror("Failed to set socket options\n");
    }

    bzero((char *)&server_addr, sizeof(server_addr)); // sets all bytes to zero

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number); // sets port number, htons(host to network short) converts to network format
    server_addr.sin_addr.s_addr = INADDR_ANY;  // allows connections from any IP address

    // attach the socket to the port and IP
    if (bind(proxy_socket_id,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0)
    {
        perror("Port is not available\n");
        exit(1);
    }
    printf("Binding on port %d\n", port_number);

    // puts the server in listening mode, ready to accept clients
    int listen_status = listen(proxy_socket_id, MAX_CLIENTS);
    if (listen_status < 0)
    {
        perror("Error in listening\n");
        exit(1);
    }
    printf("Binding on the port %d\n", port_number);

    // Tracking connected clients number
    int i = 0;
    int connected_socketid[MAX_CLIENTS];

    // proxy keeps accepting clients again and again
    while (1)
    {
        bzero((char *)&client_addr, sizeof(client_addr));
        client_len = sizeof(client_addr);

        client_socketid = accept(proxy_socket_id,                 // accept-> returns a new socket id for client
                                 (struct sockaddr *)&client_addr, // stores client IP and port
                                 (socklen_t *)&client_len);
        if (client_socketid < 0)
        {
            perror("Not able to connect");
            exit(1);
        }
        else
        {
            connected_socketid[i] = client_socketid;
        }
        struct sockaddr_in *client_pt = (struct sockaddr_in *)&client_addr; /*convert the sockaddr to sockaddr_in
                                                                            so we can access client IP address*/

        // extracting and printing client IP address and port number
        struct in_addr ip_addr = client_pt->sin_addr;       // stores IP address of the client in binary format so copy it to ip_addr
        char str[INET_ADDRSTRLEN];                          // stores IP address in string format
        inet_ntop(AF_INET, &ip_addr, str, INET_ADDRSTRLEN); // converts binary to string format

        printf("Client is connected with port number %d and IP address %s\n", ntohs(client_pt->sin_port), str); /* clientpt->sinpt contains port in network
                                                                                                                byte order, ntohs converts it to normal readable number */

        // create a thread for every client that connects
        pthread_create(&tid[i],                         // stores the ID of the newly created thread
                       NULL, thread_fn,                 // thread attribute is NULL, thread_fn is the function to be executed
                       (void *)&connected_socketid[i]); // passing the client sockid to the thread
        i++;
    }
    close(proxy_socket_id);
    return 0;
}

/*


**CACHE IMPLEMENTATION**


*/
// Searches the cache for URL, if found then update the Last used time(LRU Tracking)
cache_element *find(char *url)
{
    cache_element *site = NULL;                    // used to traverse the linked list
    int temp_lock_val = pthread_mutex_lock(&lock); // prevent other thread from modifying the cache while we are searching
    printf("Remove cache Lock acquired: %d\n", temp_lock_val);

    // traverse the linked list to find the URL
    if (head != NULL)
    {
        site = head;
        while (site != NULL)
        {
            if (strcmp(site->url, url) == 0) // compares both the URLs and if both are same then cache hit, otherwise keep traversing
            {
                printf("LRU time track before: %ld\n", site->lru_time_track);
                printf("URL found\n");
                site->lru_time_track = time(NULL); // update last access timestamp to NOW
                printf("LRU time track after: %ld\n", site->lru_time_track);
                break;
            }
            site = site->next;
        }
    }
    else
    {
        printf("URL not found\n");
    }

    temp_lock_val = pthread_mutex_unlock(&lock); // release the lock so other threads can access the cache
    // printf("Lock is unlocked\n");

    return site; // return the cache element if found, otherwise returns NULL
}

// Removes the least recently used(LRU) page from the cache
void remove_cache_element()
{
    cache_element *p;    // node before oldest
    cache_element *q;    // current node
    cache_element *temp; // oldest node found

    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Remove cache Lock acquired: %d\n", temp_lock_val);

    if (head != NULL)
    {
        for (p = head, q = head, temp = head; q->next != NULL; q = q->next) // loop through cache list
        {
            if (((q->next)->lru_time_track) < (temp->lru_time_track)) // finds the oldest element by comparing the last access timestamps
            {
                temp = q->next; // update the oldest element
                p = q;
            }
        }

        if (temp == head) // if LRU is head then update head to the next element
        {
            head = head->next;
        }

        else // previous node of LRU points to the next node of LRU, so that LRU is removed from the linked list
        {
            p->next = temp->next;
        }

        cache_size = cache_size - (temp->len) - sizeof(cache_element) - strlen(temp->url) - 1; // update cache size after removing the element
                                                                                               // sizeof(cache_element) is the size of the struct
                                                                                               // strlen(temp->url)+1 is the size of the url string stored in the struct
        free(temp->data);
        free(temp->url);
        free(temp);
    }

    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Remove cache lock is unlocked\n");
}

// Adds a new page to the cache, if cache is full then remove old one first
int add_cache_element(char *data, int size, char *url)
{
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Add cache Lock acquired: %d\n", temp_lock_val);
    int element_size = size + 1 + strlen(url) + sizeof(cache_element); // calculate size of new element to be added
                                                                       //+1 is for string terminator('\0') in data

    if (element_size > MAX_ELEMENT_SIZE) // if element size too large then unlock and return 0
    {
        temp_lock_val = pthread_mutex_unlock(&lock);
        // printf("Add cache lock is unlocked\n");
        return 0;
    }

    else
    {
        while (cache_size + element_size > MAX_SIZE) // Keep removing old elements until space exists
        {
            remove_cache_element();
        }

        cache_element *element = (cache_element *)malloc(sizeof(cache_element)); // create new cache node(allocate structure)
        element->data = (char *)malloc(size + 1);                                // allocate memory for data string
        strcpy(element->data, data);                                             // copy data to cache node

        element->url = (char *)malloc(1 + (strlen(url) * sizeof(char))); // allocate memory for url string
        strcpy(element->url, url);                                       // copy url to cache node

        element->lru_time_track = time(NULL); // set last access timestamp to NOW
        element->next = head;                 // new element points to the current head of the list
        element->len = size;                  // store the size of the data in the cache node for future reference when we need to remove it
        head = element;

        cache_size += element_size; // update cache size after adding the new element
        temp_lock_val = pthread_mutex_unlock(&lock);
        printf("Add cache lock is unlocked\n");
        return 1;
    }
    return 0;
}
