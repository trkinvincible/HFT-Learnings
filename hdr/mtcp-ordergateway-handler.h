#pragma once

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include <arpa/inet.h>
#include <iostream>
#include <thread>
#include <chrono>

class OrderGateway {
    int core_id;
    mctx_t mctx;
    int sock;

public:
    OrderGateway(int cid = 0) : core_id(cid), mctx(nullptr), sock(-1) {
        // Init mTCP per core
        mtcp_core_affinitize(core_id);
        mctx = mtcp_create_context(core_id);
        if (!mctx) {
            throw std::runtime_error("Failed to create mTCP context");
        }
    }

    ~OrderGateway() {
        if (sock >= 0) mtcp_close(mctx, sock);
        if (mctx) mtcp_destroy_context(mctx);
    }

    void connect_to(const std::string& ip, uint16_t port) {
        sock = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
        if (sock < 0) throw std::runtime_error("socket creation failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (mtcp_connect(mctx, sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            throw std::runtime_error("connect failed");
        }
        std::cout << "Connected to " << ip << ":" << port << "\n";
    }

    void send_order(const Order& o) {
        char buf[sizeof(Order)];
        o.serialize(buf);
        int ret = mtcp_write(mctx, sock, buf, sizeof(Order));
        if (ret < 0) {
            std::cerr << "Failed to send order\n";
        } else {
            std::cout << "Sent order_id=" << o.order_id
                      << " instr=" << o.instr_id
                      << " px=" << o.price
                      << " qty=" << o.qty
                      << " side=" << o.side << "\n";
        }
    }
};

MTCP_OG_TEST()
{
    // Initialize mTCP globally
    if (mtcp_init("mtcp.conf"))
    {
        std::cerr << "Failed to init mTCP\n";
        return -1;
    }

    {
        OrderGateway gw(0); // core 0
        gw.connect_to("127.0.0.1", 9000);

        Order o1{1, 1001, 101.25, 50, 'B'};
        Order o2{2, 1002, 99.75, 75, 'S'};

        gw.send_order(o1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        gw.send_order(o2);
    }

    mtcp_destroy();
}
