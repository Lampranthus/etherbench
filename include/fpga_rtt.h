#ifndef FPGA_RTT_H
#define FPGA_RTT_H

#include <stdint.h>

typedef struct {
    int sent;
    int received;
    int lost;

    double min_ms;
    double avg_ms;
    double max_ms;
    double stddev_ms;
} fpga_rtt_result_t;

int fpga_rtt_test(
    const char *fpga_ip,
    int fpga_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    fpga_rtt_result_t *result
);

int append_fpga_rtt_csv(
    const char *filename,
    const char *fpga_ip,
    int fpga_port,
    int local_port,
    int payload_size,
    const fpga_rtt_result_t *result
);

#endif