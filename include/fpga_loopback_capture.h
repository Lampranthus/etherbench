#ifndef FPGA_LOOPBACK_CAPTURE_H
#define FPGA_LOOPBACK_CAPTURE_H

#include <stdint.h>

typedef struct {
    int requested_packets;
    int payload_size;
    int sent_packets;
    int send_errors;
    int captured_packets;
    int missing_packets;
    int invalid_size_packets;
    int ignored_packets;
    uint64_t captured_bytes;
    double elapsed_s;
} fpga_loopback_capture_result_t;

int fpga_loopback_capture_run(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_data_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    const char *output_filename,
    fpga_loopback_capture_result_t *result
);

#endif
