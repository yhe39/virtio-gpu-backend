#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <error.h>
#include <unistd.h>
#include <getopt.h>

#include "utils.h"
#include "virtio_over_shmem.h"
#include "log.h"

static const char short_options[] = "d:h";

static const struct option
long_options[] = {
	{ "driver", required_argument, NULL, 'd' },
	{ "help",   no_argument,       NULL, 'h' },
	{ 0, 0, 0, 0 }
};

struct shmem_ops_info {
	const char *name;
	struct shmem_ops *ops;
};

static struct shmem_ops *shmem_ops[] = {
	&ivshm_ivshmem_ops,
	NULL
};

static bool starts_with(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void usage(FILE *fp, int argc __attribute__((unused)), char **argv)
{
	fprintf(fp,
		"Usage: %s [options] SHM-DEVICE OPTIONS\n\n"
		"Options:\n"
		"-d | --driver name   Shared memory driver name\n"
		"-h | --help          Print this message\n"
		"\n"
		"Available drivers:",
		argv[0]);

	for (struct shmem_ops **ops = shmem_ops; *ops != NULL; ops++)
		fprintf(fp, " %s", (*ops)->name);

	fprintf(fp, "\n");
}

static int infer_shmem_ops(struct virtio_backend_info *info)
{
	if (info->shmem_devpath == NULL)
		return -1;

	if (starts_with(info->shmem_devpath, "/dev/ivshm")) {
		info->shmem_ops = &ivshm_ivshmem_ops;
	} else {
		return -1;
	}

	return 0;
}

void parse_shmem_args(struct virtio_backend_info *info, int argc, char *argv[])
{
	int c = 0;
	bool found;

	while(true) {
		c = getopt_long(argc, argv,
				short_options, long_options, NULL);

		if (c < 0) {
			if (argc < optind + 2) {
				usage(stderr, argc, argv);
				exit(EXIT_FAILURE);
			}

			info->shmem_devpath = argv[optind];
			info->opts = argv[optind + 1];
			if (!info->shmem_ops && (infer_shmem_ops(info) < 0)) {
				fprintf(stderr, "Failed to infer the shared memory driver. Specify one with -d.\n");
				exit(EXIT_FAILURE);
			}
			break;
		}


		switch (c) {
		case 0: /* getopt_long() flag */
			break;

		case 'd':
			found = false;
			for (struct shmem_ops **ops = shmem_ops; *ops != NULL; ops ++) {
				if (strcmp(optarg, (*ops)->name) == 0) {
					info->shmem_ops = *ops;
					found = true;
					break;
				}
			}

			if (!found) {
				fprintf(stderr, "Unknown driver: %s\n\n", optarg);
				usage(stderr, argc, argv);
				exit(EXIT_FAILURE);
			}
			break;

		case 'h':
			usage(stdout, argc, argv);
			exit(EXIT_SUCCESS);

		default:
			usage(stderr, argc, argv);
			exit(EXIT_FAILURE);
		}
	}

	pr_info("Backend options:\n"
	       "Shared memory driver: %s\n"
	       "Shared memory device path: %s\n"
	       "Virtual device options: %s\n",
	       info->shmem_ops->name, info->shmem_devpath, info->opts);
}

void set_shmem_args(struct virtio_backend_info *info)
{
	info->shmem_devpath = "/dev/ivshm0.default";
	if (!info->shmem_ops && (infer_shmem_ops(info) < 0)) {
		fprintf(stderr, "Failed to infer the shared memory driver. Specify one with -d.\n");
		exit(EXIT_FAILURE);
	}

	pr_info("Backend options:\n"
	       "Shared memory driver: %s\n"
	       "Shared memory device path: %s\n"
	       "Virtual device options: %s\n",
	       info->shmem_ops->name, info->shmem_devpath, info->opts);
}

void *run_backend(void *data, int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	int ret;
	struct virtio_backend_info *info = (struct virtio_backend_info *)data;

	pr_info	("%s() -1.\n", __func__);

	// parse_shmem_args(info, argc, argv);
	set_shmem_args(info);

	pr_info	("%s() -2.\n", __func__);
	if (info->hook_before_init)
		info->hook_before_init(info);

	pr_info	("%s() -3.\n", __func__);
	ret = vos_backend_init(info);
	if (ret)
		error(1, ret, "Backend initialization failed.\n");

	pr_info	("%s() -4.\n", __func__);
	vos_backend_run();

	pr_info	("%s() -5.\n", __func__);
	vos_backend_deinit(info);
	pr_info	("%s() -6.\n", __func__);
	return NULL;
}

void dump_hex(void *base, int size)
{
	int i;

	pr_info("==========");
	for (i = 0; i < size; i++) {
		if ((i % 16) == 0) {
			pr_info("\n0x%02x:", i);
		}
		pr_info(" %02x", *((uint8_t*)((char*)base + i)));
	}
	pr_info("\n==========\n");
}

void dump_desc(volatile struct vring_desc *desc, int idx, bool cond __attribute__((unused)))
{
	pr_info("desc[%d] @ 0x%llx, size: %d, flags: 0x%x", idx, desc[idx].addr, desc[idx].len, desc[idx].flags);
	if (desc[idx].flags & VRING_DESC_F_NEXT) {
		pr_info(", next: %d", desc[idx].next);
	}
	pr_info("\n");
}
