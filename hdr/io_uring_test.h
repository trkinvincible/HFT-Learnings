#pragma once
#define _GNU_SOURCE
#include <iostream>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

constexpr int PORT = 12345;
constexpr const char* MULTICAST_GROUP = "239.255.0.1";
constexpr int QUEUE_DEPTH = 2;
constexpr int BUFFER_SIZE = 2048;

// Syscall wrappers
int io_uring_setup(unsigned entries, struct io_uring_params* p) {
    return syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
    return syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, nullptr, 0);
}

int setup_multicast_socket() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(PORT);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_GROUP);
    mreq.imr_interface.s_addr =  inet_addr("127.0.0.1");
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP");
        exit(1);
    }

    // 2. Set multicast loopback ON
    int loop = 1;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        perror("IP_MULTICAST_LOOP");
        exit(1);
    }

    // 3. Set multicast interface to loopback
    in_addr local_iface{};
    local_iface.s_addr = inet_addr("127.0.0.1");
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &local_iface, sizeof(local_iface)) < 0) {
        perror("IP_MULTICAST_IF");
        exit(1);
    }

    return sock;
}

std::string IO_URING_Test()
{
    int sock = setup_multicast_socket();

    // 1. Setup io_uring
    io_uring_params params = {};
    int ring_fd = io_uring_setup(QUEUE_DEPTH, &params);
    if (ring_fd < 0) {
        perror("io_uring_setup");
        exit(1);
    }
    // sqe - submission queue
    // cqe - completion queue
    size_t sqe_size = params.sq_off.array + params.sq_entries * sizeof(__u32);
    size_t cqe_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);

    void* sq_ptr = mmap(nullptr, sqe_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQ_RING);
    void* cq_ptr = mmap(nullptr, cqe_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_CQ_RING);
    void* sqes = mmap(nullptr, params.sq_entries * sizeof(io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring_fd, IORING_OFF_SQES);

    if (sq_ptr == MAP_FAILED || cq_ptr == MAP_FAILED || sqes == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    auto* sq = (uint32_t*)((char*)sq_ptr + params.sq_off.array);
    auto* sq_tail = (uint32_t*)((char*)sq_ptr + params.sq_off.tail);
    auto* sq_head = (uint32_t*)((char*)sq_ptr + params.sq_off.head);
    auto* sq_ring = (io_uring_sqe*)sqes;

    char buffer[BUFFER_SIZE] = {};
    iovec iov = { buffer, sizeof(buffer) };
    sockaddr_in src_addr{};
    msghdr msg = {};
    msg.msg_name = &src_addr;
    msg.msg_namelen = sizeof(src_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // 2. Fill SQE
    int index = *sq_tail % QUEUE_DEPTH;
    io_uring_sqe& sqe = sq_ring[index];
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_RECVMSG;
    sqe.fd = sock;
    sqe.addr = (uintptr_t)&msg;
    sqe.len = 0;
    sqe.user_data = 42;

    sq[*sq_tail % QUEUE_DEPTH] = index;
    (*sq_tail)++;

    // 3. Submit and wait
    if (io_uring_enter(ring_fd, 1, 1, IORING_ENTER_GETEVENTS) < 0) {
        perror("io_uring_enter");
        exit(1);
    }

    // 4. Get CQE
    auto* cq_head = (uint32_t*)((char*)cq_ptr + params.cq_off.head);
    auto* cq_tail = (uint32_t*)((char*)cq_ptr + params.cq_off.tail);
    auto* cqes = (io_uring_cqe*)((char*)cq_ptr + params.cq_off.cqes);

    while (*cq_head == *cq_tail) {}    // spin wait

    io_uring_cqe cqe = cqes[*cq_head % QUEUE_DEPTH];
    std::string ret{};
    if ((int)cqe.res < 0) {
        std::cerr << "recvmsg failed: " << strerror(-cqe.res) << "\n";
    } else {
        //std::cout << "Received " << cqe.res << " bytes: " << std::string(buffer, cqe.res) << "\n";
        ret = std::string(buffer, cqe.res);
    }

    (*cq_head)++;

    close(sock);
    close(ring_fd);

    return ret;
}
