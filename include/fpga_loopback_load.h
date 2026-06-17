#ifndef FPGA_LOOPBACK_LOAD_H
#define FPGA_LOOPBACK_LOAD_H

#include <stdint.h>

typedef struct {
    int packet_count;
    int payload_size;

    int sent_packets;
    int send_errors;

    int drained_packets;

    double elapsed_s;
    double packets_per_second;
    double payload_mbps;
    double estimated_wire_mbps;
} fpga_loopback_load_result_t;

int fpga_loopback_load_test(
    const char *fpga_ip,
    int fpga_data_port,
    int local_port,
    int packet_count,
    int payload_size,
    fpga_loopback_load_result_t *result
);

int append_fpga_loopback_load_csv(
    const char *filename,
    const char *fpga_ip,
    int fpga_ctrl_port,
    int fpga_data_port,
    int local_port,
    const fpga_loopback_load_result_t *result
);

#endif