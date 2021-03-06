/* Copyright(c) 2017 Jesper Dangaard Brouer, Red Hat, Inc.
 */
static const char *__doc__=
 " XDP example of cutting off flows after a packet or byte limit.";

#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>
#include <getopt.h>

#include "tools/lib/bpf/bpf.h"
#include "tools/lib/bpf/libbpf.h"
#include "libbpf.h"
#include "bpf_util.h"

#include "xdp_bypass.h"

static int if_indexes[5];
static int if_indexes_cnt=0;

static void int_exit(int sig)
{
    for(int idx=0;idx < if_indexes_cnt; idx++) {
        fprintf(stderr, "Interrupted: Removing XDP program on ifindex:%d\n",
            if_indexes[idx]);
        bpf_set_link_xdp_fd(if_indexes[idx], -1, 0);
    }
    exit(0);
}

static const struct option long_options[] = {
    {"help",    no_argument,        NULL, 'h' },
    {"ifname",  required_argument,  NULL, 'i' },
    {0, 0, NULL,  0 }
};

/* Exit return codes */

static void usage(char *argv[])
{
    int i;
    printf("\nDOCUMENTATION:\n%s\n", __doc__);
    printf("\n");
    printf(" Usage: %s (options-see-below)\n",
           argv[0]);
    printf(" Listing options:\n");
    for (i = 0; long_options[i].name != 0; i++) {
        printf(" --%-12s", long_options[i].name);
        if (long_options[i].flag != NULL)
            printf(" flag (internal value:%d)",
                   *long_options[i].flag);
        else
            printf(" short-option: -%c",
                   long_options[i].val);
        printf("\n");
    }
    printf("\n");
}


//FIXME: I should have just converted and used inet_ntop
#define V4_IP_FORMAT "%d.%d.%d.%d"
#define V4_IP_FORMAT_V(ip) \
    (ip & 0xFF), \
    ((ip >> 8) & 0xFF), \
    ((ip >> 16) & 0xFF), \
    ((ip >> 24) & 0xFF)

#define V6_IP_FORMAT "[%x:%x:%x:%x:%x:%x:%x:%x]"
#define V6_IP_FORMAT_V(ip) \
    (ntohs(ip[0] & 0xffff)), \
    (ntohs((ip[0] >> 16) & 0xffff)), \
    (ntohs(ip[1] & 0xffff)), \
    (ntohs((ip[1] >> 16) & 0xffff)), \
    (ntohs(ip[2] & 0xffff)), \
    (ntohs((ip[2] >> 16) & 0xffff)), \
    (ntohs(ip[3] & 0xffff)), \
    (ntohs((ip[3] >> 16) & 0xffff))

int bpf_map_get_next_key_and_delete(int fd, const void *key, void *next_key, int *delete)
{
    int res = bpf_map_get_next_key(fd, key, next_key);
    if (*delete) {
        bpf_map_delete_elem(fd, key);
        *delete = 0;
    }
    return res;
}


static bool expire_flows(int v4_fd, int v6_fd)
{
    struct flowv4_keys prev_key = {}, key = {};
    struct flowv6_keys prev_key6 = {}, key6 = {};
    unsigned int nr_cpus = bpf_num_possible_cpus();
    //printf("CPUS: %d\n", nr_cpus);
    struct pair values[nr_cpus];
    int i;
    struct timespec curtime;
    clock_gettime(CLOCK_MONOTONIC, &curtime);
    int flows_expired=0;
    int flows_total_v4=0, flows_total_v6=0;
    int do_expire = 0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    int delete_prev_key = 0;

    while (bpf_map_get_next_key_and_delete(v4_fd, &prev_key, &key, &delete_prev_key) == 0) {
        int res = bpf_map_lookup_elem(v4_fd, &key, values);
        if (res < 0) {
            //printf("No Entry? Flow v4: "V4_IP_FORMAT":%d -> "V4_IP_FORMAT":%d ",
            //    V4_IP_FORMAT_V(key.src), ntohs(key.port16[0]), V4_IP_FORMAT_V(key.dst), ntohs(key.port16[1]));
            prev_key = key;
            continue;
        }

        flows_total_v4++;
        do_expire = 1;
        packets = 0;
        bytes = 0;
        for (i = 0; i < nr_cpus; i++) {
            int age = curtime.tv_sec - values[i].time / 1000000000;
            packets += values[i].packets;
            bytes   += values[i].bytes;
            if (age < FLOW_TIMEOUT_SECONDS) {
                #if DEBUG
                printf("not Expired Flow v4: "V4_IP_FORMAT":%d -> "V4_IP_FORMAT":%d ",
                    V4_IP_FORMAT_V(key.src), ntohs(key.port16[0]), V4_IP_FORMAT_V(key.dst), ntohs(key.port16[1]));
                printf("i=%d t=%llu packets=%llu bytes=%llu\n", i, values[i].time / 1000000000, values[i].packets, values[i].bytes);
                #endif
                do_expire=0;
                break;
            }
        }
        if (do_expire) {
            printf("Expired Flow v4: %d "V4_IP_FORMAT":%d -> "V4_IP_FORMAT":%d ",
                key.ip_proto,
                V4_IP_FORMAT_V(key.src), ntohs(key.port16[0]), V4_IP_FORMAT_V(key.dst), ntohs(key.port16[1]));
            printf("packets=%lu bytes=%lu\n", packets, bytes);
            flows_expired++;
            flows_total_v4--;
            delete_prev_key=1;
        }
        prev_key = key;
    }
    while (bpf_map_get_next_key_and_delete(v6_fd, &prev_key6, &key6, &delete_prev_key) == 0) {
        int res = bpf_map_lookup_elem(v6_fd, &key6, values);
        if (res < 0) {
            //printf("no entry in v6 table for %d -> %d\n", key.port16[0], key.port16[1]);
            prev_key6 = key6;
            continue;
        }

        flows_total_v6++;
        do_expire = 1;
        packets = 0;
        bytes = 0;
        for (i = 0; i < nr_cpus; i++) {
            int age = curtime.tv_sec - values[i].time / 1000000000;
            packets += values[i].packets;
            bytes   += values[i].bytes;
            if (age < FLOW_TIMEOUT_SECONDS) {
                printf("not Expired Flow v6: "V6_IP_FORMAT":%d -> "V6_IP_FORMAT":%d ",
                    V6_IP_FORMAT_V(key6.src), ntohs(key6.port16[0]), V6_IP_FORMAT_V(key6.dst), ntohs(key6.port16[1]));
                printf("i=%d t=%llu packets=%llu bytes=%llu\n", i, values[i].time / 1000000000, values[i].packets, values[i].bytes);
                do_expire=0;
                break;
            }
        }
        if (do_expire) {
            printf("Expired Flow v6: %d "V6_IP_FORMAT":%d -> "V6_IP_FORMAT":%d ",
                key.ip_proto,
                V6_IP_FORMAT_V(key6.src), ntohs(key6.port16[0]), V6_IP_FORMAT_V(key6.dst), ntohs(key6.port16[1]));
            printf("packets=%lu bytes=%lu\n", packets, bytes);
            delete_prev_key=1;
            flows_expired++;
            flows_total_v6--;
        }
        prev_key6 = key6;
    }
    printf("Flows: total=%d v4=%d v6=%d expired=%d\n", flows_total_v4+flows_total_v6, flows_total_v4, flows_total_v6, flows_expired);
    fflush(stdout);

    return false;
}

static int flows_poll(struct bpf_object *pobj, int interval)
{
    struct bpf_map *flow_table_v4 = bpf_object__find_map_by_name(pobj, "flow_table_v4");
    struct bpf_map *flow_table_v6 = bpf_object__find_map_by_name(pobj, "flow_table_v6");
    if(flow_table_v4==NULL) {
        fprintf(stderr, "Can't find map flow_table_v4");
        return EXIT_FAIL;
    }
    if(flow_table_v6==NULL) {
        fprintf(stderr, "Can't find map flow_table_v6");
        return EXIT_FAIL;
    }

    int v4_fd = bpf_map__fd(flow_table_v4);
    int v6_fd = bpf_map__fd(flow_table_v6);

    while (1) {
        expire_flows(v4_fd, v6_fd);
        sleep(interval);
    }
    return EXIT_OK;
}

int bpf_prog_load_pinned(const char *file, enum bpf_prog_type type,
		  struct bpf_object **pobj, int *prog_fd, const char *pin_path)
{
	struct bpf_prog_load_attr attr;

	memset(&attr, 0, sizeof(struct bpf_prog_load_attr));
	attr.file = file;
	attr.prog_type = type;
	attr.expected_attach_type = 0;
	attr.pin_path = pin_path;

	return bpf_prog_load_xattr(&attr, pobj, prog_fd);
}
#define MAX_PROGS 32
int main(int argc, char **argv)
{
    int ret;
    struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
    char filename[256];
    int longindex = 0;
    int opt;
    char *ifname=NULL;

    struct bpf_object *pobj;
    int prog_fd[MAX_PROGS];
    int ifindex;

    snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

    /* Parse commands line args */
    while ((opt = getopt_long(argc, argv, "hi:",
                  long_options, &longindex)) != -1) {
        switch (opt) {
        case 'i':
            ifname = optarg;
            if((ifindex=if_nametoindex(ifname)) == 0) {
                perror("Can't find interface");
                return EXIT_FAIL_OPTION;
            }
            if_indexes[if_indexes_cnt++] = ifindex;
            break;
        case 'h':
        default:
            usage(argv);
            return EXIT_FAIL_OPTION;
        }
    }
    /* Required options */
    if (if_indexes_cnt==0) {
        printf("**Error**: required option --ifname missing");
        usage(argv);
        return EXIT_FAIL_OPTION;
    }

    /* Increase resource limits */
    if (setrlimit(RLIMIT_MEMLOCK, &r)) {
        perror("setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)");
        return 1;
    }

    if((ret=bpf_prog_load_pinned(filename, BPF_PROG_TYPE_XDP, &pobj, prog_fd, PIN_PATH)) < 0) {
        printf("bpf_prog_load: %s\n", strerror(ret));
        return 1;
    }
    
    if (!prog_fd[0]) {
        printf("load_bpf_file: %s\n", strerror(errno));
        return 1;
    }

    /* Remove XDP program when program is interrupted */
    signal(SIGINT, int_exit);

    for(int idx=0;idx < if_indexes_cnt; idx++) {
        if (bpf_set_link_xdp_fd(if_indexes[idx], prog_fd[0], 0) < 0) {
            printf("link set xdp fd failed\n");
            return EXIT_FAIL_XDP;
        }
    }

    if((ret=bpf_object__pin_maps(pobj, PIN_PATH) < 0)) {
        printf("bpf_object__pin_maps: %s\n", strerror(errno));
        //return 1;
    }

#define DEBUG 1
#ifdef  DEBUG
    {
    char msg[] =
        "\nDebug outout avail via:\n"
        " sudo cat /sys/kernel/debug/tracing/trace_pipe\n\n";
    printf(msg);
    }
#endif


    return flows_poll(pobj, 5);
}
