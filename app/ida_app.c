/*
 * ida_app.c - IDA userspace acquisition application
 *
 * Allocates Swath Buffers and Status Areas under /dev/shm using
 * shm_open + ftruncate + mmap(MAP_SHARED). With /dev/shm mounted
 * huge=always, the kernel will back the mapping with THP 2MB pages,
 * which is required for efficient IOMMU sg-list merging.
 *
 * Flow:
 *   1. Read config from /etc/ida_app.conf
 *   2. Verify /dev/shm is mounted with huge=always
 *   3. Allocate Swath Buffer + Status Area for each IDA channel
 *   4. Open /dev/ida_dma and call CMD_INIT (kernel pins pages)
 *   5. Main loop: CMD_START → CMD_WAIT_DONE → process data
 *   6. On exit: CMD_CLEANUP → unmap → unlink shm objects
 *
 * Build:
 *   gcc -O2 -Wall -o ida_app ida_app.c -lrt
 *   (lrt needed for shm_open on some older glibc)
 *
 * System prerequisites (run once as root):
 *   echo always > /sys/kernel/mm/transparent_hugepage/enabled
 *   mount -t tmpfs -o remount,huge=always /dev/shm
 *   # Or add to /etc/fstab:
 *   # tmpfs /dev/shm tmpfs defaults,size=50G,huge=always 0 0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* For shm_open (also in sys/mman.h above) */

/* Shared kernel/userspace interface */
#include "../include/ida_uapi.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define DEVICE_PATH         "/dev/ida_dma"
#define CONFIG_FILE         "/etc/ida_app.conf"

/*
 * Swath Buffer size: upper bound.
 * Actual data in each Swath is determined by FPGA (dmaed_line_count).
 * We allocate 10 GB so any realistic Swath fits.
 * Must be a multiple of PAGE_SIZE (4096).
 */
#define SWATH_BUF_SIZE      (10ULL * 1024 * 1024 * 1024) /10  /* 10 GB */

/*
 * Status Area size: exactly one page, matches struct ida_status_area.
 */
#define STATUS_AREA_SIZE    4096

/*
 * Maximum IDA channels supported.
 */
#define MAX_IDA_CHANNELS    4

/*
 * Default bytes per line if not specified in config.
 * 2048 samples * 1.5 bytes/sample = 3072 bytes.
 */
#define DEFAULT_BYTES_PER_LINE  3072

/* ============================================================
 * Application configuration (loaded from CONFIG_FILE)
 * ============================================================ */
struct ida_app_config {
	int      ida_count;         /* number of IDA channels (1-4)  */
	uint32_t bytes_per_line;    /* bytes per radar line           */
};

/* ============================================================
 * Per-channel resources
 * ============================================================ */
struct ida_channel_res {
	/* Swath Buffer */
	void    *swath_ptr;         /* mmap address                  */
	int      swath_fd;          /* shm fd                        */
	char     swath_name[64];    /* shm object name               */

	/* Status Area */
	struct ida_status_area *status_ptr;   /* mmap address (cast) */
	int      status_fd;         /* shm fd                        */
	char     status_name[64];   /* shm object name               */
};

/* ============================================================
 * Global state (for signal handler cleanup)
 * ============================================================ */
static volatile sig_atomic_t  g_running   = 1;
static int                    g_dev_fd    = -1;
static int                    g_ida_count = 0;
static struct ida_channel_res g_channels[MAX_IDA_CHANNELS];

/* ============================================================
 * Signal handler
 * ============================================================ */
static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ============================================================
 * Config file parser
 *
 * Format: KEY = VALUE  (one per line, # for comments)
 * Keys: IDA_COUNT, BYTES_PER_LINE
 * ============================================================ */
static int load_config(const char *path, struct ida_app_config *cfg)
{
	FILE *fp;
	char  line[256];
	char  key[64], val[64];

	/* Set defaults */
	cfg->ida_count      = 1;
	cfg->bytes_per_line = DEFAULT_BYTES_PER_LINE;

	fp = fopen(path, "r");
	if (!fp) {
		/* Config file not found is not fatal - use defaults */
		fprintf(stderr,
			"ida_app: config file %s not found, using defaults\n",
			path);
		return 0;
	}

	while (fgets(line, sizeof(line), fp)) {
		/* Skip comments and blank lines */
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;

		if (sscanf(line, "%63s = %63s", key, val) != 2)
			continue;

		if (strcmp(key, "IDA_COUNT") == 0) {
			cfg->ida_count = atoi(val);
		} else if (strcmp(key, "BYTES_PER_LINE") == 0) {
			cfg->bytes_per_line = (uint32_t)atoi(val);
		}
	}

	fclose(fp);

	/* Validate */
	if (cfg->ida_count < 1 || cfg->ida_count > MAX_IDA_CHANNELS) {
		fprintf(stderr,
			"ida_app: IDA_COUNT=%d out of range [1,%d]\n",
			cfg->ida_count, MAX_IDA_CHANNELS);
		return -1;
	}
	if (cfg->bytes_per_line == 0) {
		fprintf(stderr, "ida_app: BYTES_PER_LINE must be > 0\n");
		return -1;
	}

	printf("ida_app: config loaded:\n");
	printf("ida_app:   IDA_COUNT      = %d\n",   cfg->ida_count);
	printf("ida_app:   BYTES_PER_LINE = %u\n",   cfg->bytes_per_line);
	return 0;
}

/* ============================================================
 * Check /dev/shm is mounted with huge=always
 *
 * Reads /proc/mounts looking for a tmpfs on /dev/shm with
 * the "huge=always" option. Warns but does not abort if not
 * found - the driver will still work, just with small pages
 * (more sg entries, slightly less efficient).
 * ============================================================ */
static void check_shm_hugepage(void)
{
	FILE *fp;
	char  line[512];
	int   found = 0;

	fp = fopen("/proc/mounts", "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		/*
		 * Format: device mountpoint fstype options dump pass
		 * We look for: tmpfs /dev/shm tmpfs ...,huge=always,...
		 */
		if (strstr(line, "/dev/shm") && strstr(line, "tmpfs")) {
			if (strstr(line, "huge=always")) {
				found = 1;
				break;
			}
		}
	}
	fclose(fp);

	if (!found) {
		fprintf(stderr,
			"ida_app: WARNING: /dev/shm is not mounted with "
			"huge=always.\n"
			"ida_app: THP pages may not be used for Swath "
			"Buffers.\n"
			"ida_app: To fix (as root):\n"
			"ida_app:   mount -t tmpfs -o remount,huge=always "
			"/dev/shm\n");
	} else {
		printf("ida_app: /dev/shm is mounted with huge=always - OK\n");
	}

	/*
	 * Also check system-wide THP setting.
	 */
	fp = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
	if (fp) {
		char buf[64] = {0};
		if (fread(buf, 1, sizeof(buf) - 1, fp) > 0) {
			if (!strstr(buf, "[always]") &&
			    !strstr(buf, "[madvise]")) {
				fprintf(stderr,
					"ida_app: WARNING: THP is disabled "
					"system-wide.\n"
					"ida_app:   echo always > "
					"/sys/kernel/mm/transparent_hugepage/"
					"enabled\n");
			}
		}
		fclose(fp);
	}
}

/* ============================================================
 * Allocate one buffer in /dev/shm
 *
 * Uses shm_open + ftruncate + mmap(MAP_SHARED).
 * With /dev/shm mounted huge=always, the kernel will
 * transparently back the mapping with 2MB THP pages.
 *
 * @name    : shm object name (e.g. "/ida_swath_0")
 * @size    : allocation size in bytes (must be PAGE_SIZE multiple)
 * @out_fd  : returns the shm file descriptor (caller must close)
 * @out_ptr : returns the mmap'd pointer
 *
 * Returns 0 on success, -1 on failure.
 * ============================================================ */
static int alloc_shm(const char *name, size_t size,
		     int *out_fd, void **out_ptr)
{
	int   fd;
	void *ptr;

	/* Remove any stale object from a previous run */
	shm_unlink(name);

	fd = shm_open(name, O_CREAT | O_RDWR, 0600);
	if (fd < 0) {
		fprintf(stderr, "ida_app: shm_open(%s) failed: %s\n",
			name, strerror(errno));
		return -1;
	}

	if (ftruncate(fd, (off_t)size) < 0) {
		fprintf(stderr,
			"ida_app: ftruncate(%s, %zu) failed: %s\n",
			name, size, strerror(errno));
		close(fd);
		shm_unlink(name);
		return -1;
	}

	/*
	 * MAP_SHARED: required so kernel can pin the pages.
	 * MAP_POPULATE: fault in all pages now so pin_user_pages
	 *               in kernel doesn't encounter unmapped pages.
	 */
	ptr = mmap(NULL, size,
		   PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_POPULATE,
		   fd, 0);
	if (ptr == MAP_FAILED) {
		fprintf(stderr,
			"ida_app: mmap(%s, %zu) failed: %s\n",
			name, size, strerror(errno));
		close(fd);
		shm_unlink(name);
		return -1;
	}

	/*
	 * Touch every page to ensure physical pages are allocated
	 * before handing the virtual address to the kernel driver.
	 * MAP_POPULATE should have done this, but be explicit.
	 * For a 10 GB buffer this takes a moment (~1-2 seconds).
	 */
	{
		volatile char *p = (volatile char *)ptr;
		size_t         off;
		/*
		 * Touch one byte per 4KB page.
		 * If THP is active, pages will be promoted to 2MB
		 * transparently - we don't need to touch every 2MB.
		 */
		for (off = 0; off < size; off += 4096)
			p[off] = 0;
	}

	*out_fd  = fd;
	*out_ptr = ptr;
	return 0;
}

/* ============================================================
 * Free one /dev/shm buffer
 * ============================================================ */
static void free_shm(const char *name, void *ptr, size_t size, int fd)
{
	if (ptr && ptr != MAP_FAILED)
		munmap(ptr, size);
	if (fd >= 0)
		close(fd);
	if (name && name[0])
		shm_unlink(name);
}

/* ============================================================
 * Allocate all channel buffers
 * ============================================================ */
static int alloc_all_channels(int ida_count)
{
	int i, ret;

	for (i = 0; i < ida_count; i++) {
		struct ida_channel_res *ch = &g_channels[i];

		/* Init to safe defaults */
		ch->swath_fd  = -1;
		ch->status_fd = -1;
		ch->swath_ptr  = NULL;
		ch->status_ptr = NULL;

		/* Swath Buffer name: /ida_swath_0, /ida_swath_1, ... */
		snprintf(ch->swath_name, sizeof(ch->swath_name),
			 "/ida_swath_%d", i);

		/* Status Area name: /ida_status_0, /ida_status_1, ... */
		snprintf(ch->status_name, sizeof(ch->status_name),
			 "/ida_status_%d", i);

		/* Allocate Swath Buffer */
		ret = alloc_shm(ch->swath_name, SWATH_BUF_SIZE,
				&ch->swath_fd,
				&ch->swath_ptr);
		if (ret) {
			fprintf(stderr,
				"ida_app: failed to alloc swath[%d]\n", i);
			return ret;
		}

		/* Allocate Status Area (regular page, no THP needed) */
		ret = alloc_shm(ch->status_name, STATUS_AREA_SIZE,
				&ch->status_fd,
				(void **)&ch->status_ptr);
		if (ret) {
			fprintf(stderr,
				"ida_app: failed to alloc status[%d]\n", i);
			return ret;
		}

		printf("ida_app: channel[%d] swath=%p status=%p\n",
		       i, ch->swath_ptr, (void *)ch->status_ptr);
	}

	return 0;
}

/* ============================================================
 * Free all channel buffers
 * ============================================================ */
static void free_all_channels(int ida_count)
{
	int i;

	for (i = 0; i < ida_count; i++) {
		struct ida_channel_res *ch = &g_channels[i];

		free_shm(ch->swath_name,  ch->swath_ptr,
			 SWATH_BUF_SIZE,  ch->swath_fd);
		free_shm(ch->status_name, (void *)ch->status_ptr,
			 STATUS_AREA_SIZE, ch->status_fd);

		ch->swath_ptr  = NULL;
		ch->status_ptr = NULL;
		ch->swath_fd   = -1;
		ch->status_fd  = -1;
	}
}

/* ============================================================
 * Process one completed Swath
 *
 * Replace this stub with real data processing logic.
 * At this point:
 *   - swath_ptr[0 .. actual_bytes-1] contains valid radar data
 *   - status_ptr has been written by FPGA (dmaed_line_count etc.)
 *   - actual_bytes = result.dmaed_line_count * bytes_per_line
 * ============================================================ */
static void process_swath(int ida,
			   void *swath_ptr,
			   const struct ida_status_area *status_ptr,
			   const struct ida_wait_result *result)
{
	printf("ida_app: [IDA %d] Swath complete:\n", ida);
	printf("ida_app:   dmaed_line_count = %lld\n",
	       (long long)result->dmaed_line_count);
	printf("ida_app:   actual_bytes     = %llu (%.1f MB)\n",
	       (unsigned long long)result->actual_bytes,
	       (double)result->actual_bytes / (1024.0 * 1024.0));
	printf("ida_app:   crc_error        = %d\n",   result->crc_error);
	printf("ida_app:   fpga_state       = %d\n",   result->state);

	if (result->crc_error > 0)
		fprintf(stderr,
			"ida_app: [IDA %d] WARNING: %d CRC errors!\n",
			ida, result->crc_error);

	/*
	 * Zero-copy data access:
	 * swath_ptr is the direct /dev/shm mapping.
	 * No data was copied - the FPGA wrote directly into these pages.
	 *
	 * Verify first few bytes of data (debug only):
	 */
	if (result->actual_bytes >= 16) {
		const uint8_t *d = (const uint8_t *)swath_ptr;
		printf("ida_app:   first 8 bytes = "
		       "%02x %02x %02x %02x %02x %02x %02x %02x\n",
		       d[0], d[1], d[2], d[3],
		       d[4], d[5], d[6], d[7]);
	}

	/*
	 * Direct read of Status Area (FPGA wrote this via DMA):
	 * status_ptr->dmaed_line_count should equal result->dmaed_line_count
	 */
	(void)status_ptr;   /* suppress unused warning until real code added */

	/* TODO: add real processing here */
}

/* ============================================================
 * Cleanup: called on exit or signal
 * ============================================================ */
static void cleanup(void)
{
	/* Tell kernel to unpin pages and release IOMMU mappings */
	if (g_dev_fd >= 0) {
		if (ioctl(g_dev_fd, CMD_CLEANUP, 0) < 0)
			fprintf(stderr,
				"ida_app: CMD_CLEANUP failed: %s\n",
				strerror(errno));
		close(g_dev_fd);
		g_dev_fd = -1;
	}

	free_all_channels(g_ida_count);
	printf("ida_app: cleanup complete\n");
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[])
{
	struct ida_app_config   cfg;
	struct ida_init_param   init_param;
	struct ida_start_param  start_param;
	struct ida_wait_result  result;
	int     round = 0;
	int     i, ret;

	(void)argc;
	(void)argv;

	/* ---- 0. Signal handling ------------------------------------ */
	signal(SIGINT,  sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGHUP,  sig_handler);

	/* ---- 1. Load configuration --------------------------------- */
	ret = load_config(CONFIG_FILE, &cfg);
	if (ret)
		return EXIT_FAILURE;

	g_ida_count = cfg.ida_count;

	/* ---- 2. Check /dev/shm hugepage mount ---------------------- */
	check_shm_hugepage();

	/* ---- 3. Allocate /dev/shm buffers for all channels --------- */
	printf("ida_app: allocating %d channel(s), "
	       "swath=%.0f GB, status=%d B each\n",
	       cfg.ida_count,
	       (double)SWATH_BUF_SIZE / (1024.0 * 1024.0 * 1024.0),
	       STATUS_AREA_SIZE);

	ret = alloc_all_channels(cfg.ida_count);
	if (ret) {
		free_all_channels(cfg.ida_count);
		return EXIT_FAILURE;
	}

	/* ---- 4. Open kernel driver --------------------------------- */
	g_dev_fd = open(DEVICE_PATH, O_RDWR);
	if (g_dev_fd < 0) {
		fprintf(stderr,
			"ida_app: open(%s) failed: %s\n"
			"ida_app: Is ida_driver.ko loaded? "
			"(sudo insmod ida.ko)\n",
			DEVICE_PATH, strerror(errno));
		free_all_channels(cfg.ida_count);
		return EXIT_FAILURE;
	}

	/* ---- 5. CMD_INIT: kernel pins pages, builds IOVA table ----- */
	memset(&init_param, 0, sizeof(init_param));
	init_param.ida_count = (uint32_t)cfg.ida_count;

	for (i = 0; i < cfg.ida_count; i++) {
		init_param.bufs[i].swath_uaddr  =
			(uint64_t)(uintptr_t)g_channels[i].swath_ptr;
		init_param.bufs[i].swath_size   = SWATH_BUF_SIZE;
		init_param.bufs[i].status_uaddr =
			(uint64_t)(uintptr_t)g_channels[i].status_ptr;
	}

	ret = ioctl(g_dev_fd, CMD_INIT, &init_param);
	if (ret < 0) {
		fprintf(stderr, "ida_app: CMD_INIT failed: %s\n",
			strerror(errno));
		cleanup();
		return EXIT_FAILURE;
	}
	printf("ida_app: CMD_INIT OK - all channels initialized\n");

	/* ---- 6. Main acquisition loop ------------------------------ */
	printf("ida_app: starting acquisition loop "
	       "(Ctrl+C to stop)\n\n");

	while (g_running) {
		int ida = round % cfg.ida_count;

		/* ---- 6a. CMD_START --------------------------------- */
		memset(&start_param, 0, sizeof(start_param));
		start_param.ida_index      = (uint32_t)ida;
		start_param.bytes_per_line = cfg.bytes_per_line;

		ret = ioctl(g_dev_fd, CMD_START, &start_param);
		if (ret < 0) {
			fprintf(stderr,
				"ida_app: CMD_START[%d] failed: %s\n",
				ida, strerror(errno));
			break;
		}

		/* ---- 6b. CMD_WAIT_DONE ------------------------------ */
		/*
		 * Blocks until FPGA fires Swath End MSI-X interrupt.
		 * Returns immediately if already fired (no spurious wake).
		 * Returns -EINTR if interrupted by signal (g_running=0).
		 */
		memset(&result, 0, sizeof(result));
		ret = ioctl(g_dev_fd, CMD_WAIT_DONE, &result);
		if (ret < 0) {
			if (errno == EINTR) {
				/* Signal received - check g_running */
				if (!g_running)
					break;
				continue;
			}
			fprintf(stderr,
				"ida_app: CMD_WAIT_DONE[%d] failed: %s\n",
				ida, strerror(errno));
			break;
		}

		/* ---- 6c. Check for DMA errors ---------------------- */
		if (result.error != 0) {
			fprintf(stderr,
				"ida_app: [IDA %d] DMA error %d, "
				"skipping round %d\n",
				ida, result.error, round);
			round++;
			continue;
		}

		if (result.actual_bytes == 0) {
			fprintf(stderr,
				"ida_app: [IDA %d] zero bytes received "
				"(dmaed_lines=%lld), skipping\n",
				ida,
				(long long)result.dmaed_line_count);
			round++;
			continue;
		}

		/* ---- 6d. Process data (zero-copy) ------------------- */
		process_swath(ida,
			      g_channels[ida].swath_ptr,
			      g_channels[ida].status_ptr,
			      &result);

		round++;
	}

	printf("\nida_app: loop ended after %d round(s)\n", round);

	/* ---- 7. Cleanup -------------------------------------------- */
	cleanup();
	return EXIT_SUCCESS;
}
