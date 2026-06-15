#include "fpga_setup.h"
#include "fpga_config.h"
#include "fpga_mdio.h"
#include "config.h"

#include <stdio.h>

int fpga_setup_ksz9031_1gbe(
    const char *iface_name,
    const char *fpga_ip,
    uint8_t phy_addr,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int mdio_delay_us
)
{
    char *ops[] = ETHERBENCH_KSZ9031_1GBE_OPS_INIT;
    int op_count = ETHERBENCH_KSZ9031_1GBE_OPS_COUNT;

    printf("Checking FPGA ARP...\n");

    if (fpga_arp_setup(
            iface_name,
            fpga_ip,
            fpga_port,
            timeout_ms
        ) != 0) {
        fprintf(stderr, "FPGA ARP setup failed\n");
        return -1;
    }

    printf("FPGA ARP is ready\n");
    printf("Starting KSZ9031 1GbE configuration\n");

    if (fpga_mdio_run_sequence(
            fpga_ip,
            fpga_port,
            rx_port,
            timeout_ms,
            mdio_delay_us,
            phy_addr,
            op_count,
            ops
        ) != 0) {
        fprintf(stderr, "KSZ9031 MDIO configuration failed\n");
        return -1;
    }

    printf("KSZ9031 1GbE configuration completed\n");

    return 0;
}