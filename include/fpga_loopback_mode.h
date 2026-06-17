#ifndef FPGA_LOOPBACK_MODE_H
#define FPGA_LOOPBACK_MODE_H

int fpga_enable_loopback_if_needed(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms
);

int fpga_disable_loopback_if_needed(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms
);

#endif