#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <pthread.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

// Default settings (can be overridden by command line arguments)
char* server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This struct holds all the data each client thread needs.
 * Each thread has its own socket and epoll instance.
 */
typedef struct {
    int epoll_fd;
    int socket_fd;
    long long total_rtt_us;
    long total_messages;
    double elapsed_sec;     // total time this thread ran
    double request_rate;    // throughput for this thread
} client_thread_data_t;

/*
 * Helper function to calculate time difference in microseconds.
 */
static long long tv_diff_us(const struct timeval* start, const struct timeval* end) {
    return (end->tv_sec - start->tv_sec) * 1000000LL +
        (end->tv_usec - start->tv_usec);
}

/*
 * Makes sure we send exactly len bytes.
 * send() is not guaranteed to send everything in one call.
 */
static int send_all(int fd, const void* buf, size_t len) {
    const char* ptr = (const char*)buf;
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t n = send(fd, ptr + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;  // retry if interrupted
            return -1;
        }
        if (n == 0) return -1;
        total_sent += (size_t)n;
    }
    return 0;
}

/*
 * Similar to send_all, but for receiving.
 * Ensures we read exactly len bytes.
 */
static int recv_all(int fd, void* buf, size_t len) {
    char* ptr = (char*)buf;
    size_t total_recv = 0;

    while (total_recv < len) {
        ssize_t n = recv(fd, ptr + total_recv, len - total_recv, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;  // connection closed
        total_recv += (size_t)n;
    }
    return 0;
}

static void die(const char* msg) {
    perror(msg);
    exit(1);
}

/*
 * This is what each client thread runs.
 * It sends a fixed-size message and waits for the echo back.
 * We measure RTT for each request.
 */
void* client_thread_func(void* arg) {
    client_thread_data_t* data = (client_thread_data_t*)arg;

    struct epoll_event event, events[MAX_EVENTS];
    unsigned char send_buf[MESSAGE_SIZE];
    unsigned char recv_buf[MESSAGE_SIZE];

    // Fill the buffer with 16 bytes (no null terminator)
    for (int i = 0; i < MESSAGE_SIZE; i++)
        send_buf[i] = (unsigned char)('A' + (i % 26));

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    event.data.fd = data->socket_fd;

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD,
        data->socket_fd, &event) != 0) {
        perror("epoll_ctl add failed");
        return NULL;
    }

    struct timeval loop_start, loop_end;
    gettimeofday(&loop_start, NULL);

    for (int i = 0; i < num_requests; i++) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        if (send_all(data->socket_fd, send_buf, MESSAGE_SIZE) != 0) {
            perror("send failed");
            break;
        }

        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) { i--; continue; }
            perror("epoll_wait failed");
            break;
        }

        int success = 0;

        for (int j = 0; j < nfds; j++) {
            if (events[j].data.fd == data->socket_fd) {

                if (events[j].events & (EPOLLERR | EPOLLHUP)) {
                    fprintf(stderr, "socket closed or error\n");
                    success = -1;
                    break;
                }

                if (recv_all(data->socket_fd, recv_buf, MESSAGE_SIZE) != 0) {
                    perror("recv failed");
                    success = -1;
                    break;
                }

                success = 1;
            }
        }

        if (success <= 0) break;

        gettimeofday(&end, NULL);

        long long rtt = tv_diff_us(&start, &end);
        data->total_rtt_us += rtt;
        data->total_messages++;
    }

    gettimeofday(&loop_end, NULL);
    data->elapsed_sec =
        tv_diff_us(&loop_start, &loop_end) / 1000000.0;

    if (data->total_messages > 0 && data->elapsed_sec > 0.0)
        data->request_rate =
        (double)data->total_messages / data->elapsed_sec;
    else
        data->request_rate = 0.0;

    return NULL;
}

/*
 * Starts multiple client threads and gathers overall stats.
 */
void run_client() {
    pthread_t* threads =
        calloc(num_client_threads, sizeof(pthread_t));
    client_thread_data_t* thread_data =
        calloc(num_client_threads,
            sizeof(client_thread_data_t));

    if (!threads || !thread_data)
        die("calloc failed");

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip,
        &server_addr.sin_addr) != 1)
        die("inet_pton failed");

    for (int i = 0; i < num_client_threads; i++) {

        thread_data[i].socket_fd =
            socket(AF_INET, SOCK_STREAM, 0);
        if (thread_data[i].socket_fd < 0)
            die("socket failed");

        // Disable Nagle for small-message latency tests
        int one = 1;
        setsockopt(thread_data[i].socket_fd,
            IPPROTO_TCP, TCP_NODELAY,
            &one, sizeof(one));

        thread_data[i].epoll_fd =
            epoll_create1(0);
        if (thread_data[i].epoll_fd < 0)
            die("epoll_create1 failed");

        if (connect(thread_data[i].socket_fd,
            (struct sockaddr*)&server_addr,
            sizeof(server_addr)) != 0)
            die("connect failed");
    }

    for (int i = 0; i < num_client_threads; i++)
        pthread_create(&threads[i], NULL,
            client_thread_func,
            &thread_data[i]);

    long long total_rtt = 0;
    long total_msgs = 0;
    double total_rate = 0.0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);

        total_rtt += thread_data[i].total_rtt_us;
        total_msgs += thread_data[i].total_messages;
        total_rate += thread_data[i].request_rate;

        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }

    printf("Average RTT: %lld us\n",
        total_msgs ? total_rtt / total_msgs : 0);
    printf("Total Request Rate: %.2f messages/s\n",
        total_rate);

    free(threads);
    free(thread_data);
}

/*
 * Simple single-threaded epoll echo server.
 * Accepts connections and echoes back 16-byte messages.
 */
void run_server() {

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket");

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET,
        SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(server_ip);
    addr.sin_port = htons(server_port);

    if (bind(listen_fd,
        (struct sockaddr*)&addr,
        sizeof(addr)) != 0)
        die("bind failed");

    if (listen(listen_fd, SOMAXCONN) != 0)
        die("listen failed");

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        die("epoll_create1 failed");

    struct epoll_event event, events[MAX_EVENTS];
    event.events = EPOLLIN;
    event.data.fd = listen_fd;

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
        listen_fd, &event);

    while (1) {

        int nfds =
            epoll_wait(epoll_fd, events,
                MAX_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {

            int fd = events[i].data.fd;

            if (fd == listen_fd) {

                int client_fd =
                    accept(listen_fd, NULL, NULL);
                if (client_fd < 0) continue;

                event.events =
                    EPOLLIN | EPOLLERR | EPOLLHUP;
                event.data.fd = client_fd;

                epoll_ctl(epoll_fd,
                    EPOLL_CTL_ADD,
                    client_fd, &event);
            }
            else {

                unsigned char buffer[MESSAGE_SIZE];
                int bytes =
                    recv(fd, buffer,
                        MESSAGE_SIZE,
                        MSG_WAITALL);

                if (bytes <= 0) {
                    epoll_ctl(epoll_fd,
                        EPOLL_CTL_DEL,
                        fd, NULL);
                    close(fd);
                }
                else {
                    send_all(fd, buffer, bytes);
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {

    signal(SIGPIPE, SIG_IGN);

    if (argc > 1 &&
        strcmp(argv[1], "server") == 0) {

        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    }
    else if (argc > 1 &&
        strcmp(argv[1], "client") == 0) {

        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    }
    else {
        printf("Usage:\n");
        printf("  %s server [ip port]\n", argv[0]);
        printf("  %s client [ip port threads requests]\n",
            argv[0]);
    }

    return 0;
}
