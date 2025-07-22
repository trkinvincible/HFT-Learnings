#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <thread>
#include <chrono>

#include "io_uring_test.h"

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
