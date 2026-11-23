#include <sys/statvfs.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TIME_INTERVAL_SEC 3

struct cpu_stats {
	uint64_t user;
	uint64_t nice;
	uint64_t system;
	uint64_t idle;
	uint64_t iowait;
	uint64_t irq;
	uint64_t softirq;
	uint64_t steal;
	uint64_t guest;
	uint64_t guest_nice;

	float load_pct; // CPU load in percents
};

struct memory_stats {
	uint64_t total;
	uint64_t free;
	uint64_t available;
	uint64_t buffers;
	uint64_t cached;
	uint64_t swap_total;
	uint64_t swap_free;

	float mem_pct; // RAM usage in percents
	float swap_pct; // Swap usage in percents
};

struct network_stats {
	uint64_t rx;
	uint64_t tx;
	uint64_t timestamp; // Point in time when stats were obtained

	uint64_t speed_rx; // Receive speed in bytes/sec
	uint64_t speed_tx; // Transmit speed in bytes/sec
};

#define DISKS_MAX 32

struct disk_stats {
	char mnt[128];
	uint64_t bytes;
	uint64_t free;
	uint64_t available;
	uint64_t inodes;
	uint64_t inodes_free;

	float used_pct; // Percentage of used space
	float inodes_used_pct; // Percentage of used inodes
};

static uint64_t get_time_sec(void);

static int get_cpu_stats(struct cpu_stats *);
static int get_memory_stats(struct memory_stats *);
static int get_network_stats(struct network_stats *);
static int get_disk_stats(struct disk_stats *, int);
static int get_uptime_stats(float *);

static void calc_cpu_usage(struct cpu_stats *, struct cpu_stats *);
static void calc_memory_usage(struct memory_stats *);
static void calc_network_usage(struct network_stats *, struct network_stats *);
static void calc_disks_usage(struct disk_stats *, int);

static void print_stats(const struct cpu_stats *, const struct memory_stats *,
			const struct network_stats *, float, const struct disk_stats *,
			int);

#define timespec_diff(t1, t2) (((double)(t1).tv_sec + (t1).tv_nsec * 1e-9) - \
			       ((double)(t2).tv_sec + (t2).tv_nsec * 1e-9))

int
main(void)
{
	struct cpu_stats cpu_stats, prev_cpu_stats;
	struct memory_stats memory_stats;
	struct network_stats network_stats, prev_network_stats;
	struct disk_stats disks[DISKS_MAX];
	int n_disks;
	float uptime;
#if 0
	struct timespec t1, t2;
#endif	
	// Initialize the previous stats.
	get_cpu_stats(&prev_cpu_stats);
	get_network_stats(&prev_network_stats);
	
	for (;;) {
#if 0
		clock_gettime(CLOCK_MONOTONIC, &t1);
#endif		
		get_cpu_stats(&cpu_stats);
		get_memory_stats(&memory_stats);
		get_network_stats(&network_stats);
		n_disks = get_disk_stats(disks, DISKS_MAX);
		get_uptime_stats(&uptime);

		calc_cpu_usage(&cpu_stats, &prev_cpu_stats);
		calc_memory_usage(&memory_stats);
		calc_network_usage(&network_stats, &prev_network_stats);
		calc_disks_usage(disks, n_disks);

#if 0
		clock_gettime(CLOCK_MONOTONIC, &t2);
		printf("Exec time: %g\n", timespec_diff(t2, t1));
#endif
		print_stats(&cpu_stats, &memory_stats, &network_stats, uptime, disks,
			    n_disks);
		
		usleep(TIME_INTERVAL_SEC * 1000000);
	}
	
	return EXIT_SUCCESS;
}

static uint64_t
get_time_sec(void)
{
	struct timespec t;
	
	if (clock_gettime(CLOCK_MONOTONIC, &t) == -1)
		return 0;
	return t.tv_sec;
}

/*
 * Get CPU stats from /proc/stat.  Parse the "cpu " line which is the total over all
 * cores.
 *
 * Return -1 on error.
 */
static int
get_cpu_stats(struct cpu_stats *stats)
{
	FILE *fp;
	
	memset(stats, 0, sizeof(*stats));

	if (!(fp = fopen("/proc/stat", "r")))
		return -1;

	if (fscanf(fp, "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
		   &stats->user, &stats->nice, &stats->system, &stats->idle,
		   &stats->iowait, &stats->irq, &stats->softirq, &stats->steal,
		   &stats->guest, &stats->guest_nice) != 10) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

/*
 * Get memory stats from /proc/meminfo.
 *
 * On error return -1.
 */
static int
get_memory_stats(struct memory_stats *stats)
{
	char line[512];
	FILE *fp;
	
	memset(stats, 0, sizeof(*stats));

	if (!(fp = fopen("/proc/meminfo", "r")))
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "MemTotal: %lu kB", &stats->total) == 1)
			continue;
		if (sscanf(line, "MemFree: %lu kB", &stats->free) == 1)
			continue;
		if (sscanf(line, "MemAvailable: %lu kB", &stats->available) == 1)
			continue;
		if (sscanf(line, "Buffers: %lu kB", &stats->buffers) == 1)
			continue;
		if (sscanf(line, "Cached: %lu kB", &stats->cached) == 1)
			continue;
		if (sscanf(line, "SwapTotal: %lu kB", &stats->swap_total) == 1)
			continue;
		if (sscanf(line, "SwapFree: %lu kB", &stats->swap_free) == 1)
			continue;
	}

	if (ferror(fp)) {
		fclose(fp);
		return -1;
	}
	
	fclose(fp);
	return 0;
}

/*
 * Get network stats from /proc/net/dev.  Accumulate data over all physical
 * interfaces.
 *
 * On error return -1.
 */
static int
get_network_stats(struct network_stats *stats)
{
	char line[256], path[256 + 32];
	FILE *fp;
	char *colon, *iface;
	uint64_t rx, tx;
	
	memset(stats, 0, sizeof(*stats));

	if (!(fp = fopen("/proc/net/dev", "r")))
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (!(colon = strchr(line, ':')))
			continue;

		*colon = '\0';

		iface = line;
		while (*iface == ' ')
			++iface;

		// Skip non-physical interfaces.
		snprintf(path, sizeof(path), "/sys/class/net/%s/device", iface);
		if (access(path, F_OK) == -1)
			continue;

		if (sscanf(colon + 1, "%lu %*u %*u %*u %*u %*u %*u %*u %lu",
			   &rx, &tx) != 2)
			continue;

		stats->rx += rx;
		stats->tx += tx;
	}

	if (ferror(fp)) {
		fclose(fp);
		return -1;
	}

	stats->timestamp = get_time_sec();
	
	fclose(fp);
	return 0;
}

/*
 * Read /proc/mounts for disks.  Use statvfs() to get the disk's data.
 *
 * Return the number of disks added to the disks array.  On error return -1.
 */
static int
get_disk_stats(struct disk_stats *disks, int disks_max)
{
	char line[512], dev[128], mnt[128];
	struct statvfs vfs;
	FILE *fp;
	int count = 0;
	
	memset(disks, 0, sizeof(*disks) * disks_max);

	if (!(fp = fopen("/proc/mounts", "r")))
		return -1;

	while (fgets(line, sizeof(line), fp) && count < disks_max) {
		if (sscanf(line, "%127s %127s", dev, mnt) != 2)
			continue;

		if (statvfs(mnt, &vfs) == -1)
			continue;

		if (!vfs.f_blocks)
			continue;

		strncpy(disks[count].mnt, mnt, sizeof(disks[count].mnt) - 1);
		disks[count].mnt[sizeof(disks[count].mnt) - 1] = '\0';
		disks[count].bytes = vfs.f_blocks * vfs.f_frsize;
		disks[count].free = vfs.f_bfree * vfs.f_frsize;
		disks[count].available = vfs.f_bavail * vfs.f_frsize;
		disks[count].inodes = vfs.f_files;
		disks[count].inodes_free = vfs.f_ffree;
		
		++count;
	}

	if (ferror(fp)) {
		fclose(fp);
		return -1;
	}
	
	fclose(fp);
	return count;
}

/*
 * Get the host's up time from /proc/uptime.
 *
 * On error return -1.
 */
static int
get_uptime_stats(float *uptime)
{
	FILE *fp;
	
	*uptime = 0;

	if (!(fp = fopen("/proc/uptime", "r")))
		return -1;

	if (fscanf(fp, "%f", uptime) != 1) {
		fclose(fp);
		return -1;
	}
	
	fclose(fp);
	return 0;
}

/*
 * Calculate CPU load percentage.
 */
static void
calc_cpu_usage(struct cpu_stats *stats, struct cpu_stats *prev_stats)
{
	uint64_t total_delta, idle_delta;
	uint64_t idle, prev_idle;
	uint64_t busy, prev_busy;
	uint64_t total, prev_total;

	idle = stats->idle + stats->iowait;
	prev_idle = prev_stats->idle + prev_stats->iowait;

	busy = stats->user + stats->nice + stats->system + stats->irq +
		stats->softirq + stats->steal + stats->guest + stats->guest_nice;
	prev_busy = prev_stats->user + prev_stats->nice + prev_stats->system +
		prev_stats->irq + prev_stats->softirq + prev_stats->steal +
		prev_stats->guest + prev_stats->guest_nice;

	total = busy + idle;
	prev_total = prev_busy + prev_idle;
	
	total_delta = total > prev_total ? total - prev_total : 0;
	idle_delta = idle > prev_idle ? idle - prev_idle : 0;

	if (!total_delta)
		stats->load_pct = 0.0f;
	else
		stats->load_pct = (float)(total_delta - idle_delta) / total_delta *
			100.0f;

	*prev_stats = *stats;
}

/*
 * Calculate memory/swap usage percentage.
 */
static void
calc_memory_usage(struct memory_stats *stats)
{
	uint64_t used = stats->total - stats->available;
	uint64_t swap_used = stats->swap_total - stats->swap_free;

	if (!stats->total)
		stats->mem_pct = 0.0f;
	else
		stats->mem_pct = (float)used / stats->total * 100.0f;

	if (!stats->swap_total)
		stats->swap_pct = 0.0f;
	else
		stats->swap_pct = (float)swap_used / stats->swap_total * 100.0f;
}

/*
 * Calculate network receive/transmit speed.
 */
static void
calc_network_usage(struct network_stats *stats, struct network_stats *prev_stats)
{
	uint64_t t;
	
	stats->speed_rx = stats->speed_tx = 0;

	if (stats->timestamp > prev_stats->timestamp) {
		t = stats->timestamp - prev_stats->timestamp;
		
		if (stats->rx > prev_stats->rx)
			stats->speed_rx = (stats->rx - prev_stats->rx) / t;
		if (stats->tx > prev_stats->tx)
			stats->speed_tx = (stats->tx - prev_stats->tx) / t;
	}
	*prev_stats = *stats;
}

/*
 * Calculate disk usage percentage.
 */
static void
calc_disks_usage(struct disk_stats *disks, int n_disks)
{
	uint64_t used;
	int i;

	for (i = 0; i < n_disks; ++i) {
		if (!disks[i].available) {
			disks[i].used_pct = 0;
		} else {
			used = disks[i].bytes - disks[i].free;
			disks[i].used_pct = (float)used / disks[i].available * 100.0f;
		}

		if (!disks[i].inodes) {
			disks[i].inodes_used_pct = 0;
		} else {
			used = disks[i].inodes - disks[i].inodes_free;
			disks[i].inodes_used_pct = (float)used / disks[i].inodes *
				100.0f;
		}
	}
}

/*
 * Print stats to stdout.
 */
static void
print_stats(const struct cpu_stats *cpu_stats, const struct memory_stats *memory_stats,
	    const struct network_stats *network_stats, float uptime,
	    const struct disk_stats *disks, int n_disks)
{
	int i;
	
	printf("UP %.2f\tCPU %.2f\tMEM %.2f|%.2f\tNET %lu|%lu\n",
	       uptime, cpu_stats->load_pct, memory_stats->mem_pct,
	       memory_stats->swap_pct, network_stats->speed_rx,
	       network_stats->speed_tx);

	puts("Disks");
	for (i = 0; i < n_disks; ++i)
		printf("\t%.2f|%.2f\t%s\n", disks[i].used_pct,
		       disks[i].inodes_used_pct, disks[i].mnt);
}
