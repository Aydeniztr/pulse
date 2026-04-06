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

#define MAX_CPUS 256
#define MIN_BAR_WIDTH 10

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
    int capacity; 
    float power_w;
} Power_Stats;

// Global state for hardware discovery
char primary_net[32] = {0};
char primary_disk[32] = {0};

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

int main() {
    setlocale(LC_ALL, "");
    initscr(); start_color(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(500);
    init_pair(1, COLOR_GREEN, COLOR_BLACK); init_pair(2, COLOR_YELLOW, COLOR_BLACK);
    init_pair(3, COLOR_RED, COLOR_BLACK); init_pair(4, COLOR_CYAN, COLOR_BLACK);
    
    discover_hardware();
    CPU_Stats prev_cpu[MAX_CPUS], curr_cpu[MAX_CPUS];
    Net_Stats prev_net, curr_net;
    Disk_Stats prev_disk, curr_disk;
    int prev_n = read_cpu_stats(prev_cpu);
    read_net_stats(&prev_net); read_disk_stats(&prev_disk);

    while (getch() != 'q') {
        int rows, cols; getmaxyx(stdscr, rows, cols);
        int n_cpu = read_cpu_stats(curr_cpu);
        read_net_stats(&curr_net); read_disk_stats(&curr_disk);
        Load_Avg load; read_load_avg(&load);
        Memory_Stats mem; read_memory_stats(&mem);
        
        erase();
        attron(A_BOLD | COLOR_PAIR(4)); mvprintw(0, 0, "CPUVIEW DYNAMIC"); attroff(A_BOLD | COLOR_PAIR(4));
        mvprintw(0, cols - 20, "Load: %.2f %.2f %.2f", load.load1, load.load5, load.load15);

        // Responsive Memory Bar
        int dyn_bar = cols / 2; if (dyn_bar < MIN_BAR_WIDTH) dyn_bar = MIN_BAR_WIDTH;
        float m_p = (float)mem.used/mem.total * 100.0f;
        char m_s[64]; snprintf(m_s, 64, "%.1fG/%.1fG", (float)mem.used/1048576, (float)mem.total/1048576);
        mvprintw(1, 0, "MEM "); draw_bar(1, 5, dyn_bar, m_p, m_s, 1);

        // I/O Stats
        char rx_s[16], tx_s[16], r_s[16], w_s[16];
        format_bytes((curr_net.rx_bytes - prev_net.rx_bytes) * 2, rx_s);
        format_bytes((curr_net.tx_bytes - prev_net.tx_bytes) * 2, tx_s);
        format_bytes((curr_disk.read_sectors - prev_disk.read_sectors) * 1024, r_s);
        format_bytes((curr_disk.write_sectors - prev_disk.write_sectors) * 1024, w_s);
        mvprintw(2, 0, "NET (%s): RX: %-10s TX: %-10s", primary_net, rx_s, tx_s);
        mvprintw(3, 0, "DSK (%s): R:  %-10s W:  %-10s", primary_disk, r_s, w_s);
        mvhline(4, 0, ACS_HLINE, cols);

        // Individual CPU Cores - Scalable layout
        int cores_to_draw = n_cpu;
        if (cores_to_draw > rows - 6) cores_to_draw = rows - 6;
         
        int cpu_bar_width = cols - 15;
        for (int i = 0; i < cores_to_draw; i++) {
            unsigned long long dt = get_total_time(&curr_cpu[i]) - get_total_time(&prev_cpu[i]);
            unsigned long long di = get_idle_time(&curr_cpu[i]) - get_idle_time(&prev_cpu[i]);
            float usage = (dt > 0) ? (float)(dt - di) / (float)dt * 100.0f : 0.0f;
            mvprintw(i + 5, 0, "%-5s", curr_cpu[i].name);
            draw_bar(i + 5, 7, cpu_bar_width, usage, NULL, 1);
        }

        mvprintw(rows - 1, 0, "Press 'q' to quit | Found %d cores", n_cpu);
	mvprintw(rows - 1, cols - 23,"by Ahmet Yiğit AYDENİZ");
        refresh();
        memcpy(prev_cpu, curr_cpu, sizeof(CPU_Stats) * n_cpu);
        prev_net = curr_net; prev_disk = curr_disk;
    }
    endwin(); return 0;
}
