#define _XOPEN_SOURCE_EXTENDED 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include <locale.h>
#include <wchar.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <pwd.h>

#define MAX_CPUS 256
#define MIN_BAR_WIDTH 10
#define MAX_PROCESSES 4096

// Custom color IDs
#define COLOR_CHARCOAL 10
#define COLOR_OFFWHITE 11
#define COLOR_LAVENDER 12
#define COLOR_SAGE 13
#define COLOR_SKYBLUE 14
#define COLOR_GOLD 15
#define COLOR_ROSE 16
#define COLOR_CITRUS 17
#define COLOR_NAPLES 18
#define COLOR_BERGAMOT 19

typedef struct {
    char name[16];
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
} CPU_Stats;

typedef struct {
    float load1, load5, load15;
} Load_Avg;

typedef struct {
    unsigned long total, free, available, used;
} Memory_Stats;

typedef struct {
    char iface[32];
    unsigned long long rx_bytes, tx_bytes;
} Net_Stats;

typedef struct {
    char dev[32];
    unsigned long long read_sectors, write_sectors;
} Disk_Stats;

typedef struct {
    int has_battery;
    int capacity;          // Percentage (0-100)
    float bat_power_w;     // Battery power draw/charge in Watts
    char bat_status[16];   // "Charging", "Discharging", "Full", etc.

    int has_rapl;
    unsigned long long last_energy_uj; // Previous RAPL energy reading
    float cpu_power_w;                 // Calculated CPU power in Watts
} Power_Stats;

typedef struct {
    int pid;
    char user[32];
    char command[64];
    unsigned long long total_time;
    unsigned long long rss;
    float cpu_usage;
    float mem_usage;
    char state;
} Process_Info;

// Global state for hardware discovery and sorting
char primary_net[32] = {0};
char primary_disk[32] = {0};
char bat_path[512] = {0};
char rapl_path[256] = {0};
char current_sort = 'c';

unsigned long long get_total_time(CPU_Stats *s) {
    return s->user + s->nice + s->system + s->idle + s->iowait + 
           s->irq + s->softirq + s->steal + s->guest + s->guest_nice;
}

unsigned long long get_idle_time(CPU_Stats *s) {
    return s->idle + s->iowait;
}

int read_cpu_stats(CPU_Stats stats[MAX_CPUS]) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        CPU_Stats *s = &stats[count];
        int fields = sscanf(line, "%s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                            s->name, &s->user, &s->nice, &s->system, &s->idle,
                            &s->iowait, &s->irq, &s->softirq, &s->steal,
                            &s->guest, &s->guest_nice);
        if (fields >= 5) count++;
        if (count >= MAX_CPUS) break;
    }
    fclose(fp);
    return count;
}

void discover_hardware() {
    // Discover Net
    FILE *fp = fopen("/proc/net/dev", "r");
    if (fp) {
        char line[256], best_iface[32] = "eth0";
        unsigned long long max_bytes = 0;
        fgets(line, 256, fp); fgets(line, 256, fp); // Skip headers
        while (fgets(line, 256, fp)) {
            char iface[32]; unsigned long long rb;
            if (sscanf(line, " %[^:]: %llu", iface, &rb) == 2) {
                if (strcmp(iface, "lo") != 0 && rb > max_bytes) {
                    max_bytes = rb;
                    strncpy(best_iface, iface, 31);
                }
            }
        }
        strncpy(primary_net, best_iface, 31);
        fclose(fp);
    }

    // Discover Disk
    fp = fopen("/proc/diskstats", "r");
    if (fp) {
        char line[256], best_disk[32] = "sda";
        unsigned long long max_io = 0;
        while (fgets(line, 256, fp)) {
            char dev[32]; unsigned long long ri, wi;
            if (sscanf(line, "%*u %*u %s %*u %*u %llu %*u %*u %*u %llu", dev, &ri, &wi) == 3) {
                if ((ri + wi) > max_io && (strncmp(dev, "sd", 2) == 0 || strncmp(dev, "nvme", 4) == 0 || strncmp(dev, "vd", 2) == 0)) {
                    max_io = ri + wi;
                    strncpy(best_disk, dev, 31);
                }
            }
        }
        strncpy(primary_disk, best_disk, 31);
        fclose(fp);
    }

    // Discover RAPL
    fp = fopen("/sys/class/powercap/intel-rapl:0/energy_uj", "r");
    if (fp) {
        strncpy(rapl_path, "/sys/class/powercap/intel-rapl:0/energy_uj", 255);
        fclose(fp);
    }
    
    // Discover Battery
    DIR *dir = opendir("/sys/class/power_supply");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strncmp(ent->d_name, "BAT", 3) == 0 || strncmp(ent->d_name, "macsmc-battery", 14) == 0) {
                char type_path[512];
                snprintf(type_path, sizeof(type_path), "/sys/class/power_supply/%s/type", ent->d_name);
                FILE *tfp = fopen(type_path, "r");
                if (tfp) {
                    char type[32];
                    if (fgets(type, sizeof(type), tfp) && strncmp(type, "Battery", 7) == 0) {
                        snprintf(bat_path, sizeof(bat_path), "/sys/class/power_supply/%s", ent->d_name);
                        fclose(tfp);
                        break;
                    }
                    fclose(tfp);
                }
            }
        }
        closedir(dir);
    }
}

int read_load_avg(Load_Avg *load) {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return -1;
    fscanf(fp, "%f %f %f", &load->load1, &load->load5, &load->load15);
    fclose(fp);
    return 0;
}

int read_memory_stats(Memory_Stats *mem) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return -1;
    char label[32]; unsigned long val;
    mem->total = mem->available = 0;
    while (fscanf(fp, "%s %lu kB", label, &val) != EOF) {
        if (strcmp(label, "MemTotal:") == 0) mem->total = val;
        else if (strcmp(label, "MemAvailable:") == 0) mem->available = val;
        else if (strcmp(label, "MemFree:") == 0 && mem->available == 0) mem->available = val;
    }
    fclose(fp);
    mem->used = mem->total - mem->available;
    return 0;
}

int read_net_stats(Net_Stats *net) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, 256, fp)) {
        if (strstr(line, primary_net)) {
            char *colon = strchr(line, ':');
            sscanf(colon + 1, " %llu %*u %*u %*u %*u %*u %*u %*u %llu", &net->rx_bytes, &net->tx_bytes);
            break;
        }
    }
    fclose(fp);
    return 0;
}

int read_disk_stats(Disk_Stats *disk) {
    FILE *fp = fopen("/proc/diskstats", "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, 256, fp)) {
        char dev[32]; unsigned long long rs, ws;
        if (sscanf(line, "%*u %*u %s %*u %*u %llu %*u %*u %*u %llu", dev, &rs, &ws) == 3) {
            if (strcmp(dev, primary_disk) == 0) {
                disk->read_sectors = rs; disk->write_sectors = ws;
                break;
            }
        }
    }
    fclose(fp);
    return 0;
}

void read_battery(Power_Stats *p) {
    if (!bat_path[0]) {
        p->has_battery = 0;
        return;
    }
    p->has_battery = 1;
    char path[512];
    FILE *fp;

    // Capacity
    snprintf(path, sizeof(path), "%s/capacity", bat_path);
    fp = fopen(path, "r");
    if (fp) { if (fscanf(fp, "%d", &p->capacity) != 1) { p->capacity = 0; } fclose(fp); }

    // Status
    snprintf(path, sizeof(path), "%s/status", bat_path);
    fp = fopen(path, "r");
    if (fp) { if (fscanf(fp, "%15s", p->bat_status) != 1) { p->bat_status[0] = '\0'; } fclose(fp); }
    
    // Power Draw
    snprintf(path, sizeof(path), "%s/power_now", bat_path);
    fp = fopen(path, "r");
    if (fp) {
        long long power_uw = 0;
        if (fscanf(fp, "%lld", &power_uw) == 1) {
            p->bat_power_w = power_uw / 1000000.0f;
        } else {
            p->bat_power_w = 0.0f;
        }
        fclose(fp);
    } else {
        long long current_ua = 0, voltage_uv = 0;
        snprintf(path, sizeof(path), "%s/current_now", bat_path);
        FILE *cfp = fopen(path, "r");
        if (cfp) { if (fscanf(cfp, "%lld", &current_ua) != 1) { current_ua = 0; } fclose(cfp); }
        
        snprintf(path, sizeof(path), "%s/voltage_now", bat_path);
        FILE *vfp = fopen(path, "r");
        if (vfp) { if (fscanf(vfp, "%lld", &voltage_uv) != 1) { voltage_uv = 0; } fclose(vfp); }
        
        p->bat_power_w = (current_ua / 1000000.0f) * (voltage_uv / 1000000.0f);
    }
}

void read_cpu_power(Power_Stats *p, long elapsed_ms) {
    if (!rapl_path[0]) {
        p->has_rapl = 0;
        return;
    }
    p->has_rapl = 1;
    FILE *fp = fopen(rapl_path, "r");
    if (!fp) return;
    
    unsigned long long energy_uj = 0;
    if (fscanf(fp, "%llu", &energy_uj) == 1) {
        if (p->last_energy_uj > 0 && energy_uj >= p->last_energy_uj && elapsed_ms > 0) {
            unsigned long long diff = energy_uj - p->last_energy_uj;
            p->cpu_power_w = ((float)diff / 1000000.0f) / (elapsed_ms / 1000.0f);
        } else {
            p->cpu_power_w = 0.0f;
        }
        p->last_energy_uj = energy_uj;
    }
    fclose(fp);
}

void format_bytes(unsigned long long bytes, char *buf) {
    if (bytes > 1024 * 1024) snprintf(buf, 16, "%.1f MB/s", (float)bytes / 1048576);
    else if (bytes > 1024) snprintf(buf, 16, "%.1f KB/s", (float)bytes / 1024);
    else snprintf(buf, 16, "%llu B/s", bytes);
}

void draw_bar(int y, int x, int width, float percentage, const char *extra, int color) {
    if (width < 5) return;
    int filled = (int)((width - 2) * (percentage / 100.0f));
    mvaddch(y, x, '[');
    int c = (percentage > 85.0f) ? 3 : color;
    attron(COLOR_PAIR(c));
    for (int i = 0; i < filled; i++) mvaddstr(y, x + 1 + i, "█");
    attroff(COLOR_PAIR(c));
    for (int i = filled; i < width - 2; i++) mvaddch(y, x + 1 + i, ' ');
    mvaddch(y, x + width - 1, ']');
    if (extra) printw(" %s", extra); else printw(" %5.1f%%", percentage);
}

int compare_processes(const void *a, const void *b) {
    Process_Info *pa = (Process_Info *)a;
    Process_Info *pb = (Process_Info *)b;
    if (current_sort == 'c' || current_sort == 'C') {
        if (pb->cpu_usage > pa->cpu_usage) return 1;
        if (pb->cpu_usage < pa->cpu_usage) return -1;
    } else if (current_sort == 'm' || current_sort == 'M') {
        if (pb->mem_usage > pa->mem_usage) return 1;
        if (pb->mem_usage < pa->mem_usage) return -1;
    }
    return pa->pid - pb->pid;
}

int read_processes(Process_Info *procs, int max_procs, Process_Info *prev_procs, int prev_count, unsigned long long delta_sys, int num_cores, unsigned long mem_total) {
    static long page_size = 0;
    if (page_size == 0) page_size = sysconf(_SC_PAGESIZE);
    
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    
    struct dirent *ent;
    int count = 0;
    while ((ent = readdir(dir)) != NULL && count < max_procs) {
        if (!isdigit(ent->d_name[0])) continue;
        int pid = atoi(ent->d_name);
        
        char path[256];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        
        char line[1024];
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); continue; }
        fclose(fp);
        
        char *lparen = strchr(line, '(');
        char *rparen = strrchr(line, ')');
        if (!lparen || !rparen) continue;
        
        *lparen = '\0';
        *rparen = '\0';
        
        Process_Info *p = &procs[count];
        p->pid = pid;
        strncpy(p->command, lparen + 1, sizeof(p->command) - 1);
        p->command[sizeof(p->command) - 1] = '\0';
        
        int ppid, pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt;
        unsigned long long utime, stime;
        long cutime, cstime, priority, nice, num_threads, itrealvalue;
        unsigned long long starttime;
        unsigned long vsize;
        long rss = 0;
        
        sscanf(rparen + 2, "%c %d %d %d %d %d %u %lu %lu %lu %lu %llu %llu %ld %ld %ld %ld %ld %ld %llu %lu %ld",
               &p->state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
               &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime,
               &cutime, &cstime, &priority, &nice, &num_threads, &itrealvalue,
               &starttime, &vsize, &rss);
               
        p->total_time = utime + stime;
        p->rss = rss;
        
        p->user[0] = '\0';
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE *status_fp = fopen(path, "r");
        int uid = -1;
        if (status_fp) {
            char s_line[256];
            while (fgets(s_line, sizeof(s_line), status_fp)) {
                if (strncmp(s_line, "Uid:", 4) == 0) {
                    sscanf(s_line + 4, "%d", &uid);
                    break;
                }
            }
            fclose(status_fp);
        }
        
        if (uid >= 0) {
            struct passwd *pw = getpwuid(uid);
            if (pw) {
                strncpy(p->user, pw->pw_name, sizeof(p->user) - 1);
                p->user[sizeof(p->user) - 1] = '\0';
            } else {
                snprintf(p->user, sizeof(p->user), "%d", uid);
            }
        } else {
            strcpy(p->user, "unknown");
        }
        
        p->cpu_usage = 0.0f;
        if (prev_procs != NULL) {
            unsigned long long prev_time = 0;
            for (int i = 0; i < prev_count; i++) {
                if (prev_procs[i].pid == pid) {
                    prev_time = prev_procs[i].total_time;
                    break;
                }
            }
            if (delta_sys > 0 && p->total_time >= prev_time) {
                unsigned long long d_proc = p->total_time - prev_time;
                p->cpu_usage = (float)d_proc * num_cores * 100.0f / (float)delta_sys;
            }
        }
        
        p->mem_usage = mem_total > 0 ? (float)(rss * page_size) / (mem_total * 1024.0) * 100.0f : 0.0f;
        
        count++;
    }
    closedir(dir);
    return count;
}

void init_custom_colors() {
    if (has_colors()) {
        start_color();
        if (can_change_color()) {
            init_color(COLOR_CHARCOAL, 94, 94, 94);
            init_color(COLOR_OFFWHITE, 941, 941, 941);
            init_color(COLOR_LAVENDER, 858, 721, 1000);
            init_color(COLOR_SAGE, 517, 701, 505);
            init_color(COLOR_SKYBLUE, 286, 572, 917);
            init_color(COLOR_GOLD, 858, 721, 176);
            init_color(COLOR_ROSE, 909, 325, 364);
            init_color(COLOR_CITRUS, 996, 533, 7);
            init_color(COLOR_NAPLES, 992, 662, 74);
            init_color(COLOR_BERGAMOT, 949, 756, 474);
            
            assume_default_colors(COLOR_OFFWHITE, COLOR_CHARCOAL);
            init_pair(1, COLOR_SAGE, COLOR_CHARCOAL);
            init_pair(2, COLOR_GOLD, COLOR_CHARCOAL);
            init_pair(3, COLOR_ROSE, COLOR_CHARCOAL);
            init_pair(4, COLOR_SKYBLUE, COLOR_CHARCOAL);
            init_pair(5, COLOR_LAVENDER, COLOR_CHARCOAL);
            init_pair(6, COLOR_CITRUS, COLOR_CHARCOAL);
            init_pair(7, COLOR_NAPLES, COLOR_CHARCOAL);
            init_pair(8, COLOR_BERGAMOT, COLOR_CHARCOAL);
            init_pair(9, COLOR_OFFWHITE, COLOR_CHARCOAL);
        } else {
            // Fallback to standard colors
            init_pair(1, COLOR_GREEN, COLOR_BLACK);
            init_pair(2, COLOR_YELLOW, COLOR_BLACK);
            init_pair(3, COLOR_RED, COLOR_BLACK);
            init_pair(4, COLOR_CYAN, COLOR_BLACK);
            init_pair(5, COLOR_MAGENTA, COLOR_BLACK);
            init_pair(6, COLOR_YELLOW, COLOR_BLACK);
            init_pair(7, COLOR_WHITE, COLOR_BLACK);
            init_pair(8, COLOR_YELLOW, COLOR_BLACK);
            init_pair(9, COLOR_WHITE, COLOR_BLACK);
        }
    }
}

int main() {
    setlocale(LC_ALL, "");
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(500);
    init_custom_colors();
    
    discover_hardware();
    CPU_Stats prev_cpu[MAX_CPUS], curr_cpu[MAX_CPUS];
    Net_Stats prev_net, curr_net;
    Disk_Stats prev_disk, curr_disk;
    Process_Info prev_procs[MAX_PROCESSES], curr_procs[MAX_PROCESSES];
    
    int prev_n = read_cpu_stats(prev_cpu);
    read_net_stats(&prev_net); read_disk_stats(&prev_disk);
    
    Memory_Stats init_mem; read_memory_stats(&init_mem);
    int prev_n_procs = read_processes(prev_procs, MAX_PROCESSES, NULL, 0, 0, 1, init_mem.total);

    struct timespec last_update;
    clock_gettime(CLOCK_MONOTONIC, &last_update);

    int process_scroll_offset = 0;
    int process_selected_row = 0;
    int n_procs = prev_n_procs;
    int n_cpu = prev_n;
    
    int first_render = 1;
    Load_Avg load = {0};
    Power_Stats p_stats = {0};
    Memory_Stats mem = init_mem;
    char rx_s[16] = "0 B/s", tx_s[16] = "0 B/s", r_s[16] = "0 B/s", w_s[16] = "0 B/s";

    while (1) {
        int c = getch();
        if (c == 'q') break;
        
        int input_handled = 0;
        if (c == KEY_UP || c == 'k') {
            if (process_selected_row > 0) process_selected_row--;
            input_handled = 1;
        } else if (c == KEY_DOWN || c == 'j') {
            if (process_selected_row < n_procs - 1) process_selected_row++;
            input_handled = 1;
        } else if (c == KEY_PPAGE) {
            process_selected_row -= 10;
            if (process_selected_row < 0) process_selected_row = 0;
            input_handled = 1;
        } else if (c == KEY_NPAGE) {
            process_selected_row += 10;
            if (process_selected_row >= n_procs) process_selected_row = n_procs - 1;
            input_handled = 1;
        } else if (c == 'c' || c == 'C') {
            current_sort = 'c'; input_handled = 1;
        } else if (c == 'm' || c == 'M') {
            current_sort = 'm'; input_handled = 1;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_update.tv_sec) * 1000 + (now.tv_nsec - last_update.tv_nsec) / 1000000;

        int updated_data = 0;
        if (elapsed_ms >= 500 || first_render) {
            first_render = 0;
            last_update = now;
            
            n_cpu = read_cpu_stats(curr_cpu);
            read_net_stats(&curr_net); read_disk_stats(&curr_disk);
            read_load_avg(&load);
            read_memory_stats(&mem);
            
            unsigned long long delta_sys = get_total_time(&curr_cpu[0]) - get_total_time(&prev_cpu[0]);
            int num_cores = n_cpu > 1 ? n_cpu - 1 : 1;
            
            n_procs = read_processes(curr_procs, MAX_PROCESSES, prev_procs, prev_n_procs, delta_sys, num_cores, mem.total);
            
            qsort(curr_procs, n_procs, sizeof(Process_Info), compare_processes);
            
            float time_factor = 1000.0f / (float)(elapsed_ms > 0 ? elapsed_ms : 500);
            format_bytes((unsigned long long)((curr_net.rx_bytes - prev_net.rx_bytes) * time_factor), rx_s);
            format_bytes((unsigned long long)((curr_net.tx_bytes - prev_net.tx_bytes) * time_factor), tx_s);
            format_bytes((unsigned long long)((curr_disk.read_sectors - prev_disk.read_sectors) * 512 * time_factor), r_s);
            format_bytes((unsigned long long)((curr_disk.write_sectors - prev_disk.write_sectors) * 512 * time_factor), w_s);
            
            read_battery(&p_stats);
            read_cpu_power(&p_stats, elapsed_ms > 0 ? elapsed_ms : 500);

            updated_data = 1;
        } else if (input_handled) {
            qsort(curr_procs, n_procs, sizeof(Process_Info), compare_processes);
        } else {
            continue;
        }
        
        int rows, cols; getmaxyx(stdscr, rows, cols);
        erase();
        
        attron(A_BOLD | COLOR_PAIR(4)); mvprintw(0, 0, "CPUVIEW DYNAMIC"); attroff(A_BOLD | COLOR_PAIR(4));
        
        char header_info[128];
        int header_len = snprintf(header_info, sizeof(header_info), "Load: %.2f %.2f %.2f", load.load1, load.load5, load.load15);
        if (p_stats.has_rapl) {
            header_len += snprintf(header_info + header_len, sizeof(header_info) - header_len, " | CPU: %.1fW", p_stats.cpu_power_w);
        }
        if (p_stats.has_battery) {
            if (strncmp(p_stats.bat_status, "Charging", 8) == 0 || strncmp(p_stats.bat_status, "Full", 4) == 0) {
                header_len += snprintf(header_info + header_len, sizeof(header_info) - header_len, " | BAT: %d%% (AC)", p_stats.capacity);
            } else {
                header_len += snprintf(header_info + header_len, sizeof(header_info) - header_len, " | BAT: %d%% (-%.1fW)", p_stats.capacity, p_stats.bat_power_w);
            }
        }
        mvprintw(0, cols - header_len, "%s", header_info);

        // Responsive Memory Bar
        int dyn_bar = cols / 2; if (dyn_bar < MIN_BAR_WIDTH) dyn_bar = MIN_BAR_WIDTH;
        float m_p = (float)mem.used/mem.total * 100.0f;
        char m_s[64]; snprintf(m_s, 64, "%.1fG/%.1fG", (float)mem.used/1048576, (float)mem.total/1048576);
        mvprintw(1, 0, "MEM "); draw_bar(1, 5, dyn_bar, m_p, m_s, 1);

        // I/O Stats
        mvprintw(2, 0, "NET (%s): RX: %-10s TX: %-10s", primary_net, rx_s, tx_s);
        mvprintw(3, 0, "DSK (%s): R:  %-10s W:  %-10s", primary_disk, r_s, w_s);
        mvhline(4, 0, ACS_HLINE, cols);

        // Total CPU Bar
        unsigned long long dt_tot = get_total_time(&curr_cpu[0]) - get_total_time(&prev_cpu[0]);
        unsigned long long di_tot = get_idle_time(&curr_cpu[0]) - get_idle_time(&prev_cpu[0]);
        float usage_tot = (dt_tot > 0) ? (float)(dt_tot - di_tot) / (float)dt_tot * 100.0f : 0.0f;
        attron(A_BOLD); mvprintw(5, 0, "TOTAL"); attroff(A_BOLD);
        draw_bar(5, 7, cols - 15, usage_tot, NULL, 6);

        // Individual CPU Cores - Multi-column layout
        int columns = cols / 35; if (columns < 1) columns = 1;
        int cores_to_draw = n_cpu - 1;
        int rows_for_cores = (cores_to_draw + columns - 1) / columns;
        
        int max_rows_for_cores = rows - 6 - 12 - 1; // Leave 12 lines for processes
        if (rows_for_cores > max_rows_for_cores) {
            rows_for_cores = max_rows_for_cores;
            if (rows_for_cores < 1) rows_for_cores = 1;
        }
        
        int col_width = cols / columns;
        int cores_actually_drawn = 0;
        for (int i = 0; i < cores_to_draw && cores_actually_drawn < rows_for_cores * columns; i++) {
            int r = i / columns;
            int c = i % columns;
            if (r >= rows_for_cores) break;
            
            unsigned long long dt = get_total_time(&curr_cpu[i+1]) - get_total_time(&prev_cpu[i+1]);
            unsigned long long di = get_idle_time(&curr_cpu[i+1]) - get_idle_time(&prev_cpu[i+1]);
            float usage = (dt > 0) ? (float)(dt - di) / (float)dt * 100.0f : 0.0f;
            
            mvprintw(6 + r, c * col_width, "%-5s", curr_cpu[i+1].name);
            draw_bar(6 + r, c * col_width + 7, col_width - 10, usage, NULL, 1);
            cores_actually_drawn++;
        }

        // Process List Rendering
        int process_start_y = 6 + rows_for_cores;
        int list_height = rows - process_start_y - 2;
        if (list_height < 1) list_height = 1;
        
        if (process_selected_row < process_scroll_offset) {
            process_scroll_offset = process_selected_row;
        } else if (process_selected_row >= process_scroll_offset + list_height) {
            process_scroll_offset = process_selected_row - list_height + 1;
        }
        
        attron(A_REVERSE | COLOR_PAIR(7));
        mvprintw(process_start_y, 0, "%-7s %-10s %5s %5s %-5s %-s", "PID", "USER", 
                 (current_sort == 'c' || current_sort == 'C') ? "CPU%*" : "CPU%", 
                 (current_sort == 'm' || current_sort == 'M') ? "MEM%*" : "MEM%", 
                 "STATE", "COMMAND");
        for (int i = getcurx(stdscr); i < cols; i++) addch(' ');
        attroff(A_REVERSE | COLOR_PAIR(7));
        
        for (int i = 0; i < list_height && (i + process_scroll_offset) < n_procs; i++) {
            int p_idx = i + process_scroll_offset;
            if (p_idx == process_selected_row) attron(A_REVERSE | COLOR_PAIR(7));
            Process_Info *p = &curr_procs[p_idx];
            int cmd_width = cols - 37;
            if (cmd_width < 5) cmd_width = 5;
            mvprintw(process_start_y + 1 + i, 0, "%-7d %-10.10s %5.1f %5.1f %-5c %-.*s", 
                     p->pid, p->user, p->cpu_usage, p->mem_usage, p->state, cmd_width, p->command);
                     
            if (p_idx == process_selected_row) attroff(A_REVERSE | COLOR_PAIR(7));
        }

        // Footer
        attron(COLOR_PAIR(8));
        mvprintw(rows - 1, 0, "Sort: [c]CPU [m]MEM | Scroll: Up/Down | 'q' to quit");
        mvprintw(rows - 1, cols - 23,"by Ahmet Yiğit AYDENİZ");
        attroff(COLOR_PAIR(8));
        
        refresh();
        if (updated_data) {
            memcpy(prev_cpu, curr_cpu, sizeof(CPU_Stats) * n_cpu);
            prev_net = curr_net; prev_disk = curr_disk;
            memcpy(prev_procs, curr_procs, sizeof(Process_Info) * n_procs);
            prev_n_procs = n_procs;
        }
    }
    endwin(); return 0;
}
