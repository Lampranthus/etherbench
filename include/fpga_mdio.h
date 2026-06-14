#ifndef FPGA_MDIO_H
#define FPGA_MDIO_H

#include <stdint.h>

typedef struct {
    uint8_t reg;
    uint16_t value;
    uint8_t raw[3];
} fpga_mdio_read_result_t;

int fpga_mdio_read(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int delay_us,
    uint8_t phy_addr,
    uint8_t reg_addr,
    fpga_mdio_read_result_t *result
);

int fpga_mdio_write(
    const char *fpga_ip,
    int fpga_port,
    int delay_us,
    uint8_t phy_addr,
    uint8_t reg_addr,
    uint16_t value
);

int fpga_mdio_run_sequence(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int delay_us,
    uint8_t phy_addr,
    int op_count,
    char **ops
);

#endif