#pragma once

/*
 * tick_to_trade_dpdk_mtcp.cpp
 * Minimal example of multicast market data receiver (4KB datagrams)
 * using DPDK for kernel bypass and mTCP for TCP control plane.
 * Optimized with rte_memcpy for high-throughput TickerData processing.
 */

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <chrono>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>

constexpr uint16_t RX_RING_SIZE = 1024;
constexpr uint16_t NUM_MBUFS = 8192;
constexpr uint16_t MBUF_CACHE_SIZE = 250;
constexpr uint16_t BURST_SIZE = 32;
constexpr size_t MAX_PKT_SIZE = 4096;

struct TickerData {
    uint64_t ts_ns;
    uint32_t instr_id;
    double price;
    uint32_t qty;
} __attribute__((__packed__));

class TickToTradeHandler
{
public:
    TickToTradeHandler(const char* multicastAddr, uint16_t port_id)
    : dpdk_nic_id(port_id), myMulticastAddr(multicastAddr) {}

    std::string ret;

    bool init()
	{
        const char *dpdk_argv[] = {
            "hft-programs",                                // program name
            //"-l", "0-1",
            "-n", "4",                              // number of memory channels
            "--vdev=net_af_packet0,iface=veth0"     // virtual TAP NIC bound to veth0
        };
        int dpdk_argc = std::size(dpdk_argv);

        // Initialize DPDK
        int ret = rte_eal_init(dpdk_argc, const_cast<char**>(dpdk_argv));
        if (ret < 0)
		{
            std::cerr << "Failed to initialize DPDK EAL\n";
            return false;
        }
        if (!init_port()) return false;
        return true;
    }

    void run()
	{
        std::vector<rte_mbuf*> bufs(BURST_SIZE);
        while (true)
        {
            uint16_t nb_rx = rte_eth_rx_burst(dpdk_nic_id, 0, bufs.data(), BURST_SIZE);
            if (nb_rx)
            {
                process_packet(bufs[0]);
                rte_pktmbuf_free(bufs[0]);
                break;
            }
        }
    }

private:
    uint16_t dpdk_nic_id; // this is equivalent to a socket ID.
    const char* myMulticastAddr;

    bool init_port()
	{
        rte_eth_conf port_conf = {};

        if (rte_eth_dev_configure(dpdk_nic_id, 1, 1, &port_conf) != 0)
		{
            std::cerr << "Failed to configure port\n";
            return false;
        }

        // Enable multicast reception for 239.1.1.1 → 01:00:5e:01:01:01
        //struct rte_ether_addr mac_addr;
        //rte_eth_macaddr_get(dpdk_nic_id, &mcast_addr); // this is to get the MAC programmatically
        //rte_ether_unformat_addr(myMulticastAddr, &mac_addr);
        // if (rte_eth_dev_mac_addr_add(dpdk_nic_id, &mac_addr, 0) < 0)
        // {
        //     // This is similar to setsockopt(IP_ADD_MEMBERSHIP).
        //     std::cerr << "Failed to add multicast MAC filter\n";
        // }
        // below will accept all multicast
        rte_eth_allmulticast_enable(dpdk_nic_id);
        rte_eth_promiscuous_enable(dpdk_nic_id);

        rte_mempool* mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (!mbuf_pool)
		{
            std::cerr << "Failed to create mbuf pool\n";
            return false;
        }

        if (rte_eth_rx_queue_setup(dpdk_nic_id, 0, RX_RING_SIZE, rte_eth_dev_socket_id(dpdk_nic_id), nullptr, mbuf_pool) < 0)
		{
            std::cerr << "Failed to setup RX queue\n";
            return false;
        }
        if (rte_eth_dev_start(dpdk_nic_id) < 0)
		{
            std::cerr << "Failed to start port\n";
            return false;
        }

        return true;
    }

    void process_packet(rte_mbuf* mbuf)
	{
        size_t offset = 0;
        struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);

        struct rte_ipv4_hdr* ip_hdr = (struct rte_ipv4_hdr*)(eth_hdr + 1);
        struct rte_udp_hdr* udp_hdr = (struct rte_udp_hdr*)(ip_hdr + 1);
        // Check multicast dest + port
        if (ip_hdr->dst_addr != rte_cpu_to_be_32(0xEFFF0001) && // 239.255.255.1 (example)
            udp_hdr->dst_port != rte_cpu_to_be_16(12345))
        {
            return;
        }

        uint16_t payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);

        // Process multiple TickerData entries efficiently
        while (offset + sizeof(TickerData) <= payload_len)
		{
            TickerData* td = (TickerData*)(udp_hdr + 1);
            handle_tick(*td);
            offset += sizeof(TickerData);
        }
    }

    void handle_tick(const TickerData& td)
	{
        auto ts = std::chrono::nanoseconds(td.ts_ns);
        std::stringstream ss;
        ss << "Tick: instr=" << td.instr_id << " price=" << td.price << " qty=" << td.qty << " ts_ns=" << ts.count();
        ret += ss.str();
    }
};

std::string DPDK_TBT_Test(const std::string& multicastAddr, int port_id)
{
    TickToTradeHandler handler(multicastAddr.data(), port_id);
    if (!handler.init())
    {
        return "EXIT_FAILURE";
    }
    handler.run();
    return handler.ret;
}
