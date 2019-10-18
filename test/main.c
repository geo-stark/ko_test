#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include "../ko_test_ioctl.h"

#define DEFAULT_DEV "/dev/ko_test_device"

static void randomize()
{
	struct timespec tp = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &tp);
	srand(tp.tv_nsec);
}

static int cmd_add(int fd, int argc, char **argv)
{
	ko_test_node node;
	int ret;

	switch (argc)
	{
	case 0:
		node.key = rand();
		node.value = rand();
		printf("using random key-value pair: %d %d\n", node.key, node.value);
		break;
	case 2:
		node.key = atoi(argv[0]);
		node.value = atoi(argv[1]);
		break;
	default:
		printf("usage: add [<key> <value>]\n");
		return -1;
	}

	ret = ioctl(fd, KO_TEST_IOCTL_ADD, &node);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_ADD");
		return -1;
	}
	return 0;
}

static int cmd_get(int fd, int argc, char **argv)
{
	ko_test_node node;
	int ret;

	if (argc != 1)
	{
		printf("usage: get <key>\n");
		return -1;
	}

	node.key = atoi(argv[0]);
	ret = ioctl(fd, KO_TEST_IOCTL_GET, &node);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_GET");
		return -1;
	}
	printf("key-value pair: %d %d\n", node.key, node.value);
	return 0;
}

static int cmd_del(int fd, int argc, char **argv)
{
	ko_test_node node;
	int ret;

	if (argc != 1)
	{
		printf("usage: del <key>\n");
		return -1;
	}

	node.key = atoi(argv[0]);
	ret = ioctl(fd, KO_TEST_IOCTL_DEL, &node);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_DEL");
		return -1;
	}
	printf("key-value pair %d deleted\n", node.key);
	return 0;
}

int main(int argc, char **argv)
{
	int fd, arg_int, ret;

	randomize();
	fd = open(DEFAULT_DEV, O_RDWR);
	if (fd == -1)
	{
		perror("open");
		return EXIT_FAILURE;
	}

	char version[KO_TEST_MAX_VERSION_SIZE] = "";
	ret = ioctl(fd, KO_TEST_IOCTL_VERSION, version);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_VERSION");
		return EXIT_FAILURE;
	}
	printf("module version: %s\n", version);

	unsigned int node_count = 0;
	ret = ioctl(fd, KO_TEST_IOCTL_COUNT, &node_count);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_COUNT");
		return EXIT_FAILURE;
	}
	printf("current key-value pairs count: %d\n", node_count);

	if (argc > 1)
	{
		int res;

		if (strcmp(argv[1], "add") == 0)
			res = cmd_add(fd, argc - 2, argv + 2);
		else if (strcmp(argv[1], "del") == 0)
			res = cmd_del(fd, argc - 2, argv + 2);
		else if (strcmp(argv[1], "get") == 0)
			res = cmd_get(fd, argc - 2, argv + 2);
		else
			printf("unknown command: %s\n", argv[1]);
	}

	close(fd);
	return EXIT_SUCCESS;
}
