#pragma once

#define _GNU_SOURCE
#include <liburing.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>

#define PORT         12345
#define GROUP        "239.255.0.1"
#define QUEUE_DEPTH  256
#define BUF_COUNT    4
#define BUF_SIZE     2048
#define BUF_GROUP    0

namespace N2
{
    int setup_multicast_socket()
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) { perror("socket"); exit(1); }

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_port   = htons(PORT);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0)
        {
            perror("bind"); exit(1);
        }

        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(GROUP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        {
            perror("IP_ADD_MEMBERSHIP"); exit(1);
        }

        // 2. Set multicast loopback ON
        int loop = 1;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
        {
            perror("IP_MULTICAST_LOOP");
            exit(1);
        }

        // 3. Set multicast interface to loopback
        in_addr local_iface{};
        local_iface.s_addr = inet_addr("127.0.0.1");
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &local_iface, sizeof(local_iface)) < 0)
        {
            perror("IP_MULTICAST_IF");
            exit(1);
        }

        return sock;
    }
}

std::string IO_URING_Test_ZERO_COPY()
{
    // Buffer pool
    static char bufs[BUF_COUNT][BUF_SIZE] __attribute__((aligned(64)));

    int sock = N2::setup_multicast_socket();

    // 1. Setup io_uring
    struct io_uring ring;
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0)
    {
        perror("io_uring_queue_init"); exit(1);
    }

    // 2. Register buffers
    struct iovec iovecs[BUF_COUNT];
    for (int i = 0; i < BUF_COUNT; i++)
    {
        iovecs[i].iov_base = bufs[i];
        iovecs[i].iov_len  = BUF_SIZE;
    }
    if (io_uring_register_buffers(&ring, iovecs, BUF_COUNT) < 0)
    {
        perror("io_uring_register_buffers"); exit(1);
    }

    // 3. Allocate a buffer ring to recycle used buffers
    struct io_uring_buf_ring *br;
    int err = 0;
    br = io_uring_setup_buf_ring(&ring, BUF_COUNT, BUF_GROUP, 0, &err);
    if (br == MAP_FAILED)
    {
        perror("io_uring_setup_buf_ring"); exit(1);
    }
    // Initially give all buffers to the kernel
    unsigned idx = 0;
    for (int i = 0; i < BUF_COUNT; i++)
    {
        io_uring_buf_ring_add(br, bufs[i], BUF_SIZE, i, idx++, BUF_COUNT);
    }
    io_uring_buf_ring_advance(br, BUF_COUNT);

    // 4. Post first recvmsg SQE
    struct msghdr msg = {0};
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recvmsg(sqe, sock, &msg, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUF_GROUP;
    sqe->user_data = 1;
    io_uring_submit(&ring);

    std::stringstream result{};

    int readCount = BUF_COUNT;

    // 5. Event loop
    while (true)
    {
        if (readCount-- == 0)
        {
            break;
        }
        std::cout << "readCount: " << readCount << std::endl;
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) continue;

        if (cqe->res > 0)
        {
            // Extract buffer ID
            unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            printf("Got packet (%d bytes) in buf %u: %.*s\n",
                   cqe->res, bid, cqe->res, bufs[bid]);
            result << bufs[bid] << " | " ;

            // Recycle this buffer back to kernel
            idx = 0;
            io_uring_buf_ring_add(br, bufs[bid], BUF_SIZE, bid, idx, BUF_COUNT);
            io_uring_buf_ring_advance(br, 1);

            // Re-post recvmsg SQE
            sqe = io_uring_get_sqe(&ring);
            memset(&msg, 0, sizeof(msg));
            io_uring_prep_recvmsg(sqe, sock, &msg, 0);
            sqe->flags |= IOSQE_BUFFER_SELECT;
            sqe->buf_group = BUF_GROUP;
            sqe->user_data = 1;
            io_uring_submit(&ring);
        }
        else
        {
            fprintf(stderr, "recvmsg failed: %s\n", strerror(-cqe->res));
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    close(sock);
    io_uring_queue_exit(&ring);

    return result.str();
}
