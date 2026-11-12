#ifndef SYSINFO_H
#define SYSINFO_H

#include <stddef.h>

/**
 * get_system_info_string:
 * Populates buffer with a formatted ASCII report containing:
 * - OS name, kernel release, machine architecture
 * - Server uptime (formatted in days, hours, mins, secs)
 * - Total RAM and free RAM (in MB)
 * - 1, 5, 15 minute CPU load averages
 * - Server PID and current active client count
 * 
 * Returns length of string written, or -1 on error.
 */
int get_system_info_string(char *buffer, size_t max_len, int active_clients);

#endif // SYSINFO_H
