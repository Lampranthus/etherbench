#define _DEFAULT_SOURCE

#include "fpga_loopback_mode.h"
#include "fpga_stats.h"
#include "fpga_ctrl.h"

#include <stdio.h>
#include <unistd.h>

#define FPGA_LOOPBACK_WAIT_US 500000

int fpga_enable_loopback_if_needed(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms
)
{
    fpga_stats_t stats;

    printf("Checking FPGA loopback mode...\n");

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats\n");
        return -1;
    }

    if (stats.mode.loopback) {
        printf("FPGA loopback is already enabled\n");
        return 0;
    }

    printf("FPGA loopback is disabled. Enabling loopback...\n");

    /*
     * Important:
     * Your FPGA command appears to toggle loopback.
     * So we only send it when regstats says loopback is disabled.
     */
    if (fpga_ctrl_enable_loopback(fpga_ip, fpga_ctrl_port) != 0) {
        fprintf(stderr, "Could not send loopback command\n");
        return -1;
    }

    usleep(FPGA_LOOPBACK_WAIT_US);

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats after loopback command\n");
        return -1;
    }

    if (!stats.mode.loopback) {
        fprintf(stderr, "FPGA loopback did not enable\n");
        return -1;
    }

    printf("FPGA loopback enabled\n");

    return 0;
}

int fpga_disable_loopback_if_needed(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms
)
{
    fpga_stats_t stats;

    printf("Checking FPGA loopback mode before exit...\n");

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats\n");
        return -1;
    }

    if (!stats.mode.loopback) {
        printf("FPGA loopback is already disabled\n");
        return 0;
    }

    printf("FPGA loopback is enabled. Disabling loopback...\n");

    /*
     * Same toggle command, but only sent if loopback is currently enabled.
     */
    if (fpga_ctrl_enable_loopback(fpga_ip, fpga_ctrl_port) != 0) {
        fprintf(stderr, "Could not send loopback command\n");
        return -1;
    }

    usleep(FPGA_LOOPBACK_WAIT_US);

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats after loopback command\n");
        return -1;
    }

    if (stats.mode.loopback) {
        fprintf(stderr, "FPGA loopback did not disable\n");
        return -1;
    }

    printf("FPGA loopback disabled\n");

    return 0;
}