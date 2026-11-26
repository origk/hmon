#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct cpu_ticks {
	uint64_t user;
	uint64_t nice;
	uint64_t system;
	uint64_t idle;
	uint64_t iowait;
	uint64_t irq;
	uint64_t softirq;
	uint64_t steal;
};

struct net_stats {
	uint64_t rx_bytes;
	uint64_t tx_bytes;
};

static int get_cpu_ticks(struct cpu_ticks *out);
static void calc_cpu_usage(const struct cpu_ticks *prev, const struct cpu_ticks *curr, float *cpu_pct, float *iowait_pct);
static int get_total_net_stats(struct net_stats *out);

#define TIME_SEC 3

int
main(void)
{
	struct cpu_ticks prev_cpu = {0}, curr_cpu = {0};
	float cpu_pct, iowait_pct;
	struct net_stats prev_net = {0}, curr_net = {0};
	uint64_t rx_speed, tx_speed;

	for (;;) {
		get_cpu_ticks(&curr_cpu);
		calc_cpu_usage(&prev_cpu, &curr_cpu, &cpu_pct, &iowait_pct);
		prev_cpu = curr_cpu;

		get_total_net_stats(&curr_net);
		rx_speed = curr_net.rx_bytes > prev_net.rx_bytes ? (curr_net.rx_bytes - prev_net.rx_bytes) / TIME_SEC : 0;
		tx_speed = curr_net.tx_bytes > prev_net.tx_bytes ? (curr_net.tx_bytes - prev_net.tx_bytes) / TIME_SEC : 0;
		prev_net = curr_net;

		printf("CPU: load %.2f%%, iowait %.2f%%\n",
		       cpu_pct,
		       iowait_pct);
		printf("NET: rx speed %lu B/s, tx speed %lu B/s\n",
		       rx_speed,
		       tx_speed);
		puts("");

		usleep(TIME_SEC * 1000000);
	}

	return EXIT_SUCCESS;
}

/*
 * Reads the overal CPU counters from the first line of /proc/stat.
 *
 * TODO: iowait is unreliable, will need a better way.
 */
static int
get_cpu_ticks(struct cpu_ticks *out)
{
	FILE *fp;
	int n;

	memset(out, 0, sizeof(*out));

	if (!(fp = fopen("/proc/stat", "r")))
		return -1;

	n = fscanf(fp, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
	       &out->user,
	       &out->nice,
	       &out->system,
	       &out->idle,
	       &out->iowait,
	       &out->irq,
	       &out->softirq,
	       &out->steal);

	fclose(fp);
	return n == 8 ? 0 : -1;
}

/*
 * Calculates percentage of the CPU load and iowait between two mesurements.
 */
static void
calc_cpu_usage(const struct cpu_ticks *prev, const struct cpu_ticks *curr, float *cpu_pct, float *iowait_pct)
{
	uint64_t prev_idle, curr_idle;
	uint64_t prev_busy, curr_busy;
	uint64_t prev_total, curr_total;
	uint64_t total_d, idle_d, iowait_d;

	prev_idle = prev->idle + prev->iowait;
	curr_idle = curr->idle + curr->iowait;

	prev_busy = prev->user + prev->nice + prev->system + prev->irq + prev->softirq + prev->steal;
	curr_busy = curr->user + curr->nice + curr->system + curr->irq + curr->softirq + curr->steal;

	prev_total = prev_idle + prev_busy;
	curr_total = curr_idle + curr_busy;

	total_d = curr_total > prev_total ? curr_total - prev_total : 0;
	idle_d = curr_idle > prev_idle ? curr_idle - prev_idle : 0;
	iowait_d = curr->iowait > prev->iowait ? curr->iowait - prev->iowait : 0;

	if (!total_d) {
		*cpu_pct = *iowait_pct = 0.0f;
		return;
	}
	*cpu_pct = (float)(total_d - idle_d) * 100.0f / (float)total_d;
	*iowait_pct = (float)iowait_d * 100.0f / (float)total_d;
}

/*
 * Accumulates total bytes over all physical interfaces read from /proc/net/dev.
 */
static int
get_total_net_stats(struct net_stats *out)
{
	char line[256], path[256 + 32];
	FILE *fp;
	char *colon, *iface;
	uint64_t rx, tx;

	memset(out, 0, sizeof(*out));

	if (!(fp = fopen("/proc/net/dev", "r")))
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (!(colon = strchr(line, ':')))
			continue;
		*colon = '\0';

		iface = line;
		while (*iface == ' ')
			++iface;

		snprintf(path, sizeof(path), "/sys/class/net/%s/device", iface);
		if (access(path, F_OK) == -1)
			continue;

		rx = tx = 0;
		if (sscanf(colon + 1, "%lu %*u %*u %*u %*u %*u %*u %*u %lu",
			   &rx,
			   &tx) != 2)
			continue;

		out->rx_bytes += rx;
		out->tx_bytes += tx;
	}

	fclose(fp);
	return 0;
}
