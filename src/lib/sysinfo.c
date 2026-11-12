#include "sysinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

int get_system_info_string(char *buffer, size_t max_len, int active_clients) {
    if (!buffer || max_len == 0) {
        return -1;
    }

    // 1. uname info
    struct utsname un;
    if (uname(&un) < 0) {
        strncpy(un.sysname, "Unknown", sizeof(un.sysname));
        strncpy(un.release, "Unknown", sizeof(un.release));
        strncpy(un.machine, "Unknown", sizeof(un.machine));
    }

    // 2. sysinfo for uptime and memory
    struct sysinfo si;
    long uptime = 0;
    unsigned long total_ram_mb = 0;
    unsigned long free_ram_mb = 0;

    if (sysinfo(&si) == 0) {
        uptime = si.uptime;
        unsigned long mem_unit = si.mem_unit ? si.mem_unit : 1;
        total_ram_mb = (si.totalram * mem_unit) / (1024 * 1024);
        free_ram_mb = (si.freeram * mem_unit) / (1024 * 1024);
    }

    long days = uptime / 86400;
    long hours = (uptime % 86400) / 3600;
    long mins = (uptime % 3600) / 60;
    long secs = uptime % 60;

    // 3. /proc/loadavg
    double load1 = 0.0, load5 = 0.0, load15 = 0.0;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp) {
        if (fscanf(fp, "%lf %lf %lf", &load1, &load5, &load15) != 3) {
            load1 = load5 = load15 = 0.0;
        }
        fclose(fp);
    }

    // 4. Format into dashboard
    int written = snprintf(buffer, max_len,
        "================ [ SERVER SYSTEM INFO ] ================\n"
        "  OS / Kernel      : %s %s (%s)\n"
        "  Server Uptime    : %ldd %ldh %ldm %lds\n"
        "  CPU Load Average : %.2f, %.2f, %.2f (1, 5, 15 min)\n"
        "  Memory Usage     : %lu MB free / %lu MB total\n"
        "  Worker PID       : %d\n"
        "  Active Clients   : %d\n"
        "========================================================",
        un.sysname, un.release, un.machine,
        days, hours, mins, secs,
        load1, load5, load15,
        free_ram_mb, total_ram_mb,
        (int)getpid(),
        active_clients
    );

    if (written < 0 || (size_t)written >= max_len) {
        return -1;
    }

    return written;
}
