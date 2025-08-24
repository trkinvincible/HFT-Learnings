#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <thread>
#include <chrono>

#include "io_uring_test.h"
#include "dpdk-tbt-handler.h"
#include "mtcp-ordergateway-handler.h"

TEST_CASE("IO_URING")
{
    // sudo ip link set lo multicast on
    using namespace std::chrono_literals;
    std::thread t{[](){
        //system("sudo ip link set lo multicast on");
        std::this_thread::sleep_for(3s);
        system("echo \"hello\" | socat -v - UDP-DATAGRAM:239.255.0.1:12345,sp=54321,bind=127.0.0.1");
    }};
    t.detach();
    REQUIRE(IO_URING_Test()=="hello\n");
}

TEST_CASE("DPDK_TBT")
{
    using namespace std::chrono_literals;
    std::thread t{[](){
        // DPDk needs huge pages setup
        // sudo mkdir /mnt/C406655E0665528A/Code-Factory/MINE/QtCreator-Projects/practice/hft-programs/huge
        // echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
        // sudo mount -t hugetlbfs nodev /mnt/C406655E0665528A/Code-Factory/MINE/QtCreator-Projects/practice/hft-programs/huge/

        // I want to do multicast on loop back like above but DPDK DPDK EAL can only bind to physical NICs or vdevs.
        // They operate at Layer 2 (Ethernet), not Layer 3 (IP/UDP).
        // Hence create a virtual NIC like below
        // Why cant i create a Wifi virtual ports. DPDK does not support Wi-Fi NICs
        // {
        //      ip link add veth0 type veth peer name veth1
        //      ip addr add 10.0.0.1/24 dev veth0
        //      ip addr add 10.0.0.2/24 dev veth1
        //      ip maddr add 01:00:5e:7f:00:01 dev veth1 -- this is hex for for 239.255.0.1
        //      ip link set veth0 up
        //      ip link set veth1 up
        // }
        // Below is defined in the code
        //--vdev=net_tap0,iface=veth0 →
        //create a DPDK tap-backed NIC named net_tap0.
        //bind it to the kernel network interface veth0.

        std::this_thread::sleep_for(3s);
        // system("sudo ./dpdk-script.sh");
        system("cat ticker_packet.bin | socat -u - UDP-DATAGRAM:239.255.0.1:12345,ip-multicast-if=10.0.0.2");
    }};
    t.detach();
    /*
    Every device you create with --vdev=... is registered by DPDK as an Ethernet device.
    DPDK enumerates devices starting at port_id = 0, then 1, etc.
    So if you do:
    ./build/hft-programs -n 4 --vdev=net_af_packet0,iface=veth0 --vdev=net_af_packet1,iface=veth1
    then:
    port_id 0 → net_af_packet0 (bound to veth0)
    port_id 1 → net_af_packet1 (bound to veth1)
     */
    REQUIRE(DPDK_TBT_Test(/*"7a:e3:16:0f:41:69"*/"3e:a4:7f:02:54:af", 0)=="Tick: instr=2 price=20.8 qty=20 ts_ns=0");
}

TEST_CASE("MTCP_OG_TEST")
{
    // netmap is no more compatible with later linux kernel 6+ for virtio and not experimenting dpdk either
    // So i am falling back to psio default mode for mTCP.
    /*
    *   sudo apt install linux-headers-$(uname -r) build-essential
        git clone https://github.com/luigirizzo/netmap.git
        cd netmap
        ./configure --kernel-sources=/lib/modules/$(uname -r)/build --no-drivers=virtio_net.c
        make -j$(nproc)
        sudo make install

        Load the module:
        sudo modprobe netmap
     */
    using namespace std::chrono_literals;
    std::thread t{[](){
        std::this_thread::sleep_for(3s);
        // running a localhost server at port 9000 to which mTCP will write the data to. incoming data will be written to a output.dat file
        // for inspection
        system("socat TCP-LISTEN:9000,bind=localhost,fork open:output.dat,creat");
    }};
    t.detach();
    MTCP_OG_TEST();
    // REQUIRE(output.dat);
}