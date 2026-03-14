/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/*
Please specify the group members here

# Student #1: Biplav Khatiwada
# Student #2: Hamblet Arroyo Alvarado
# Student #3:

*/

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

// Basic configuration
#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define WINDOW_SIZE 32
#define MAX_REQUESTS 100000

// Default settings (can be overridden by command line arguments)
char* server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

// Packet format used between client and server
typedef struct {
    int seq_num;
    int client_id;
    char payload[MESSAGE_SIZE];
} packet_t;

// Data used by each client thread
typedef struct {
    int thread_id;
    int base;
    int nextseqnum;
    int epoll_fd;
    int socket_fd;
    struct sockaddr_in server_addr;
    packet_t send_buffer[MAX_REQUESTS];
    int acked[MAX_REQUESTS];
    long long total_rtt_us;
    long total_messages;
    long tx_cnt;
    long rx_cnt;
    long lost_pkt_cnt;
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

// Ensuring all the bytes are sent
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

// Ensuring all bytes are recieving
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

// Print error and exit
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
    packet_t send_pkt;
    packet_t recv_pkt;

    memset(&send_pkt, 0, sizeof(send_pkt));
    memset(&recv_pkt, 0, sizeof(recv_pkt));

    // Fill the buffer with 16 bytes (no null terminator)
    for (int i = 0; i < MESSAGE_SIZE; i++)
        send_pkt.payload[i] = (char)('A' + (i % 26));

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

    int sent_requests = 0;

// Sends request until target is reached
    while (sent_requests < num_requests) {
        int batch_count = 0;
        int recv_target = 0;
        struct timeval batch_start, batch_end;
        gettimeofday(&batch_start, NULL);

    while (batch_count < WINDOW_SIZE && sent_requests < num_requests) {

        send_pkt.seq_num = data->nextseqnum;
        send_pkt.client_id = data->thread_id;
        data->send_buffer[data->nextseqnum] = send_pkt;

        if (sendto(data->socket_fd, &send_pkt, sizeof(send_pkt), 0,
           (struct sockaddr*)&data->server_addr,
           sizeof(data->server_addr)) < 0) {
           perror("sendto failed");
           break;
	}

        data->tx_cnt++;
        sent_requests++;
        batch_count++;
        data->nextseqnum++;
    }

    recv_target = batch_count;

    int received_in_batch = 0;

   // Waiting for responses
    while (received_in_batch < recv_target) {
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) { continue; }
            perror("epoll_wait failed");
            break;
        }

       // Retransmit outstanding packets on timeout
        if (nfds == 0) {
           for (int k = data->base; k < data->nextseqnum; k++) {
               if (sendto(data->socket_fd, &data->send_buffer[k],
                  sizeof(packet_t), 0,
                  (struct sockaddr*)&data->server_addr,
                  sizeof(data->server_addr)) < 0) {
                  perror("retransmit sendto failed");
                  break;
               }
           }
           continue;
        }

        int success = 0;

        for (int j = 0; j < nfds; j++) {
            if (events[j].data.fd == data->socket_fd) {

                if (events[j].events & (EPOLLERR | EPOLLHUP)) {
                    fprintf(stderr, "socket closed or error\n");
                    success = -1;
                    break;
                }

                ssize_t bytes = recvfrom(data->socket_fd, &recv_pkt, sizeof(recv_pkt), 0, NULL, NULL);

                if (bytes < 0) {
                    perror("recvfrom failed");
                    success = -1;
                    break;
                }

               // Mark packet as acknowledged
                if (recv_pkt.seq_num >= 0 && recv_pkt.seq_num < MAX_REQUESTS) {
                    data->acked[recv_pkt.seq_num] = 1;
                }

                while (data->base < data->nextseqnum && data->acked[data->base]) {
                    data->base++;
                }

                data->rx_cnt++;
                received_in_batch++;
                success = 1;
            }
        }

        if (success <= 0) {
            break;
        }
    }
        gettimeofday(&batch_end, NULL);

        long long rtt = tv_diff_us(&batch_start, &batch_end);
        data->total_rtt_us += rtt;
        data->total_messages+= received_in_batch;
    }

    gettimeofday(&loop_end, NULL);
    data->elapsed_sec =
        tv_diff_us(&loop_start, &loop_end) / 1000000.0;

    if (data->total_messages > 0 && data->elapsed_sec > 0.0)
        data->request_rate =
        (double)data->total_messages / data->elapsed_sec;
    else
        data->request_rate = 0.0;

    data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;

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
        thread_data[i].thread_id = i;
        thread_data[i].base = 0;
        thread_data[i].nextseqnum = 0;

        memset(thread_data[i].acked, 0, sizeof(thread_data[i].acked));

        thread_data[i].socket_fd =
            socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0)
            die("socket failed");

	thread_data[i].server_addr = server_addr;

        // Keep latency-related socket option setup
        int one = 1;
        setsockopt(thread_data[i].socket_fd,
            IPPROTO_TCP, TCP_NODELAY,
            &one, sizeof(one));

        thread_data[i].epoll_fd =
            epoll_create1(0);
        if (thread_data[i].epoll_fd < 0)
            die("epoll_create1 failed");

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

        long long avg_thread_rtt = 0;
        if (thread_data[i].total_messages > 0) {
           avg_thread_rtt =
               thread_data[i].total_rtt_us / thread_data[i].total_messages;
        }

        printf("Thread %d: tx=%ld rx=%ld lost=%ld messages=%ld avg_rtt=%lld us rate=%.2f messages/s\n",
            i,
            thread_data[i].tx_cnt,
            thread_data[i].rx_cnt,
            thread_data[i].lost_pkt_cnt,
            thread_data[i].total_messages,
            avg_thread_rtt,
            thread_data[i].request_rate);

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

// Simple UPD echo server.
void run_server() {

    int listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
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
               packet_t pkt;
               struct sockaddr_in client_addr;
               socklen_t client_len = sizeof(client_addr);

               int bytes = recvfrom(listen_fd, &pkt, sizeof(pkt), 0,
                   (struct sockaddr*)&client_addr, &client_len);

               if (bytes < 0) {
                  perror("recvfrom failed");
               }
               else {
                   if (sendto(listen_fd, &pkt, bytes, 0,
                      (struct sockaddr*)&client_addr, client_len) < 0) {
                      perror("sendto failed");
                   }
               }
            }
        }
    }
}

// Program entry point
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
