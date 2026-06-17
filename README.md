# etherbench

 1. Clone the repository enter to etherbench and do make.
 2. Setup the namespace with the interface to be tested.
    ```bash
    # example to test the eth0 interface on the raspberry pi
    # Create namespace
    sudo ip netns add eth_ns
    
    # Configure eth_ns
    sudo ip link set eth0 netns eth_ns
    sudo ip netns exec eth_ns ip addr add 192.168.1.11/24 dev eth0
    sudo ip netns exec eth_ns ip link set eth0 up
    sudo ip netns exec eth_ns ip link set lo up
    
    # Open a terminal inside the namespace
    sudo ip netns exec eth_ns bash
    ```
 3. Set up the system for test the conection with the fpga.
    a. Start with the ARP test for confirm the fpga is in the net.
    ```bash
    ./etherbench fpga-arp <iface> <fpga_ip> [fpga_port]
    ```
    ```bash
    # default example on namespace eth_ns
    ./etherbench fpga-arp eth0 192.168.1.12 55555
    ```
    b. If ARP set success, set up the PHY registers.
    ```bash
    ./etherbench fpga-setup <iface> <fpga_ip> <phy> [fpga_port]
    ```
    ```bash
    # default example on namespacew eth_ns
    ./etherbench fpga-setup eth0 192.168.1.12 7 55555
    ```
    c. If setup success, set up the destination ip on the fpga for recive the udp on the device.
    ```bash
    ./etherbench fpga-net <fpga_ip> <field> <value> [fpga_port]
    ```
    ```bash
    # example on the namespace eth_ns
    ./etherbench fpga-net 192.168.1.12 dest-ip 192.168.1.11
    ```
    d. If setup succeced, the intarface can recive regstats.
    ```bash
    ./etherbench fpga <fpga_ip> [fpga_port] [rx_port]
    ```
    ```bash
    # example on the namespace eth_ns
    ./etherbench fpga 192.168.1.12 55555 9999
    ```
    c. if the regstats suceced, the setup is already you can start the tests.
4. Running the rtt test.
   ```bash
   ./etherbench fpga-rtt <fpga_ip> <packets> <payload_size> [loopback_port] [local_port] [fpga_ctrl_port]
   ```
   ```bash
   # example for namespace eth_ns
   ./etherbench fpga-rtt 192.168.1.12 1000 1440 1234 9999 55555
   ```
5. Running the loopback test.
   ```bash
   ./etherbench fpga-loopback-test <iface> <fpga_ip> <packets> <payload_size> [data_port] [local_port] [ctrl_port]
   ```
   ```bash
   # example for namespace eth_ns
   ./etherbench fpga-loopback-test eth0 192.168.1.12 10000000 1440 1234 9999 55555
   ```
