#ifndef FPGA_SETUP_H
#define FPGA_SETUP_H

#include <stdint.h>

int fpga_setup_ksz9031_1gbe(
    const char *iface_name,
    const char *fpga_ip,
    uint8_t phy_addr,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int mdio_delay_us
);

#endif