mkdir -p /mnt/C406655E0665528A/Code-Factory/MINE/QtCreator-Projects/practice/hft-programs/huge
echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
mount -t hugetlbfs nodev /mnt/C406655E0665528A/Code-Factory/MINE/QtCreator-Projects/practice/hft-programs/huge/
ip link add veth0 type veth peer name veth1
ip addr add 10.0.0.1/24 dev veth0
ip addr add 10.0.0.2/24 dev veth1
ip maddr add 01:00:5e:7f:00:01 dev veth1
ip link set veth0 up
ip link set veth1 up
        