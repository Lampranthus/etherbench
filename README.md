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
6. Runnign trasmition test.
   ```bash
   ./etherbench fpga-tx-test <iface> <fpga_ip> <packets> <payload_size> <mode> [ctrl_port] [local_port]
   ```
   ```bash
   # example for namespace eth_ns
   ./etherbench fpga-tx-test eth0 192.168.1.12 10000000 1440 random 55555 9999
   ```

7. Running payload sweeps and plots.
   ```bash
   scripts/etherbench_sweep.py run --iface <iface> --fpga-ip <fpga_ip>
   ```
   ```bash
   # default sweep:
   # - payloads: 256, 320, 384, ... 1408, 1440
   # - repetitions: 3 per payload
   # - RTT: 1000 packets
   # - loopback: 1000000 packets
   # - FPGA TX: 1000000 packets
   scripts/etherbench_sweep.py run --iface eth0 --fpga-ip 192.168.1.12
   ```
   The script writes each run in a new `results/sweep_YYYYMMDD_HHMMSS`
   directory, including raw etherbench CSV logs, summary CSV files and SVG plots:
   `rtt_payload_sweep.svg`, `loopback_goodput_payload_sweep.svg`,
   `tx_goodput_payload_sweep.svg`, `loopback_loss_payload_sweep.svg`,
   `tx_loss_payload_sweep.svg` and `pps_payload_sweep.svg`.

   To preview commands without sending packets:
   ```bash
   scripts/etherbench_sweep.py run --iface eth0 --fpga-ip 192.168.1.12 --dry-run
   ```

   To use a custom payload list:
   ```bash
   scripts/etherbench_sweep.py run --iface eth0 --fpga-ip 192.168.1.12 --payloads 256,512,1024,1440
   ```

   To rebuild summaries or plots from an existing result directory:
   ```bash
   scripts/etherbench_sweep.py summarize --output-dir results/sweep_YYYYMMDD_HHMMSS
   scripts/etherbench_sweep.py plot --output-dir results/sweep_YYYYMMDD_HHMMSS
   ```
