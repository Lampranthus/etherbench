#ifndef FPGA_TX_TEST_H
#define FPGA_TX_TEST_H

#include <stdint.h>

typedef struct {
    int requested_packets;
    int payload_size;
    char mode[32];

    int received_packets;
    int lost_packets;

    double elapsed_s;
    double trigger_to_last_s;
    double payload_goodput_mbps;
    double estimated_wire_mbps;
} fpga_tx_test_result_t;

typedef struct {
    int requested_packets;
    int payload_size;
    char mode[32];

    int captured_packets;
    int lost_packets;
    int invalid_size_packets;
    int ignored_packets;
    uint64_t captured_bytes;
    double elapsed_s;
    double trigger_to_last_s;
} fpga_tx_capture_result_t;

int fpga_tx_test_run(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int packet_count,
    int payload_size,
    const char *mode,
    fpga_tx_test_result_t *result
);

int fpga_tx_capture_run(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    const char *mode,
    const char *output_filename,
    fpga_tx_capture_result_t *result
);

int append_fpga_tx_test_csv(
    const char *filename,
    const char *iface_name,
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    const fpga_tx_test_result_t *result
);

int verify_fpga_tx_config(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    const char *mode
);

#endif
