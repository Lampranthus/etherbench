#include "net_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#define LINE_MAX_LEN 2048

static int file_exists(const char *filename)
{
    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        return 0;
    }

    fclose(file);
    return 1;
}

static int parse_ip_values(const char *line, ip_stats_t *ip)
{
    int matched;

    matched = sscanf(
        line,
        "Ip: %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
        &ip->forwarding,
        &ip->default_ttl,
        &ip->in_receives,
        &ip->in_hdr_errors,
        &ip->in_addr_errors,
        &ip->forw_datagrams,
        &ip->in_unknown_protos,
        &ip->in_discards,
        &ip->in_delivers,
        &ip->out_requests,
        &ip->out_discards,
        &ip->out_no_routes,
        &ip->reasm_timeout,
        &ip->reasm_reqds,
        &ip->reasm_oks,
        &ip->reasm_fails,
        &ip->frag_oks,
        &ip->frag_fails,
        &ip->frag_creates
    );

    if (matched != 19) {
        return -1;
    }

    return 0;
}

static int parse_udp_values(const char *line, udp_stats_t *udp)
{
    int matched;

    matched = sscanf(
        line,
        "Udp: %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64
        " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
        &udp->in_datagrams,
        &udp->no_ports,
        &udp->in_errors,
        &udp->out_datagrams,
        &udp->rcvbuf_errors,
        &udp->sndbuf_errors,
        &udp->in_csum_errors,
        &udp->ignored_multi,
        &udp->mem_errors
    );

    if (matched != 9) {
        return -1;
    }

    return 0;
}

int read_net_stats(net_stats_t *stats)
{
    FILE *file;
    char line[LINE_MAX_LEN];
    char previous_line[LINE_MAX_LEN];

    int found_ip = 0;
    int found_udp = 0;

    if (stats == NULL) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    memset(previous_line, 0, sizeof(previous_line));

    file = fopen("/proc/net/snmp", "r");

    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        /*
         * /proc/net/snmp has pairs of lines:
         *
         * Ip:  field field field...
         * Ip:  value value value...
         *
         * Udp: field field field...
         * Udp: value value value...
         */

        if (strncmp(previous_line, "Ip:", 3) == 0 &&
            strncmp(line, "Ip:", 3) == 0) {
            if (parse_ip_values(line, &stats->ip) == 0) {
                found_ip = 1;
            }
        }

        if (strncmp(previous_line, "Udp:", 4) == 0 &&
            strncmp(line, "Udp:", 4) == 0) {
            if (parse_udp_values(line, &stats->udp) == 0) {
                found_udp = 1;
            }
        }

        snprintf(previous_line, sizeof(previous_line), "%s", line);
    }

    fclose(file);

    if (!found_ip || !found_udp) {
        return -1;
    }

    return 0;
}

void print_net_stats(const net_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    printf("\n");
    printf("=== IP layer stats ===\n");
    printf("IP InReceives:       %" PRIu64 "\n", stats->ip.in_receives);
    printf("IP InHdrErrors:      %" PRIu64 "\n", stats->ip.in_hdr_errors);
    printf("IP InAddrErrors:     %" PRIu64 "\n", stats->ip.in_addr_errors);
    printf("IP InUnknownProtos:  %" PRIu64 "\n", stats->ip.in_unknown_protos);
    printf("IP InDiscards:       %" PRIu64 "\n", stats->ip.in_discards);
    printf("IP InDelivers:       %" PRIu64 "\n", stats->ip.in_delivers);
    printf("IP OutRequests:      %" PRIu64 "\n", stats->ip.out_requests);
    printf("IP OutDiscards:      %" PRIu64 "\n", stats->ip.out_discards);
    printf("IP OutNoRoutes:      %" PRIu64 "\n", stats->ip.out_no_routes);
    printf("IP ReasmReqds:       %" PRIu64 "\n", stats->ip.reasm_reqds);
    printf("IP ReasmOKs:         %" PRIu64 "\n", stats->ip.reasm_oks);
    printf("IP ReasmFails:       %" PRIu64 "\n", stats->ip.reasm_fails);

    printf("\n");
    printf("=== UDP layer stats ===\n");
    printf("UDP InDatagrams:     %" PRIu64 "\n", stats->udp.in_datagrams);
    printf("UDP NoPorts:         %" PRIu64 "\n", stats->udp.no_ports);
    printf("UDP InErrors:        %" PRIu64 "\n", stats->udp.in_errors);
    printf("UDP OutDatagrams:    %" PRIu64 "\n", stats->udp.out_datagrams);
    printf("UDP RcvbufErrors:    %" PRIu64 "\n", stats->udp.rcvbuf_errors);
    printf("UDP SndbufErrors:    %" PRIu64 "\n", stats->udp.sndbuf_errors);
    printf("UDP InCsumErrors:    %" PRIu64 "\n", stats->udp.in_csum_errors);
    printf("UDP IgnoredMulti:    %" PRIu64 "\n", stats->udp.ignored_multi);
    printf("UDP MemErrors:       %" PRIu64 "\n", stats->udp.mem_errors);
}

int append_net_stats_csv(const char *filename, const net_stats_t *stats)
{
    FILE *file;
    time_t now;
    int write_header;

    if (filename == NULL || stats == NULL) {
        return -1;
    }

    write_header = !file_exists(filename);

    file = fopen(filename, "a");

    if (file == NULL) {
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,"
            "ip_in_receives,ip_in_hdr_errors,ip_in_addr_errors,"
            "ip_in_unknown_protos,ip_in_discards,ip_in_delivers,"
            "ip_out_requests,ip_out_discards,ip_out_no_routes,"
            "ip_reasm_reqds,ip_reasm_oks,ip_reasm_fails,"
            "udp_in_datagrams,udp_no_ports,udp_in_errors,udp_out_datagrams,"
            "udp_rcvbuf_errors,udp_sndbuf_errors,udp_in_csum_errors,"
            "udp_ignored_multi,udp_mem_errors\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,"
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 "\n",
        now,
        stats->ip.in_receives,
        stats->ip.in_hdr_errors,
        stats->ip.in_addr_errors,
        stats->ip.in_unknown_protos,
        stats->ip.in_discards,
        stats->ip.in_delivers,
        stats->ip.out_requests,
        stats->ip.out_discards,
        stats->ip.out_no_routes,
        stats->ip.reasm_reqds,
        stats->ip.reasm_oks,
        stats->ip.reasm_fails,
        stats->udp.in_datagrams,
        stats->udp.no_ports,
        stats->udp.in_errors,
        stats->udp.out_datagrams,
        stats->udp.rcvbuf_errors,
        stats->udp.sndbuf_errors,
        stats->udp.in_csum_errors,
        stats->udp.ignored_multi,
        stats->udp.mem_errors
    );

    fclose(file);

    return 0;
}