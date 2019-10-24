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
#define MAX_STRING_SIZE 32

static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

static void randomize()
{
	struct timespec tp = {0, 0};
	clock_gettime(CLOCK_MONOTONIC, &tp);
	srand(tp.tv_nsec);
}

static char *realloc_string(char *str, int size)
{
    char *ptr = realloc(str, size + 1);
    ptr[size] = 0;
    return ptr;
}

static void alloc_random_string(char **out_str, int *out_size)
{
    int i;
    int size = (rand() % MAX_STRING_SIZE) + 1;
    char *str = realloc_string(NULL, size);

    for (i = 0; i < size; i++)
	str[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    *out_size = size;
    *out_str = str;
}

static int cmd_add(int fd, int argc, char **argv)
{
	ko_test_node node;
	int ret;

	switch (argc)
	{
	case 0:
		alloc_random_string(&node.key, &node.key_size);
		alloc_random_string(&node.value, &node.value_size);
		printf("using random key-value pair: %s %s\n", node.key, node.value);
		break;
	case 2:
		node.key = argv[0];
		node.key_size = strlen(argv[0]);
		node.value = argv[1];
		node.value_size = strlen(argv[1]);
		break;
	default:
		printf("usage: add [<key> <value>]\n");
		return -1;
	}

	ret = ioctl(fd, KO_TEST_IOCTL_ADD, &node);
	if (argc == 0)
	{
	    free(node.key);
	    free(node.value);
	}
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
	memset(&node, 0, sizeof(node));
	node.key = argv[0];
	node.key_size = strlen(argv[0]);

	ret = ioctl(fd, KO_TEST_IOCTL_GET, &node);
	if (ret == -1)
	{
	    if (errno != ENOSPC)
	    {
		perror("ioctl - KO_TEST_IOCTL_GET");
		return -1;
	    }
	    node.value = realloc_string(NULL, node.value_size);
	    ret = ioctl(fd, KO_TEST_IOCTL_GET, &node);
	    if (ret == -1)
	    {
		perror("ioctl - KO_TEST_IOCTL_GET");
		free(node.value);
		return -1;
	    }
	}
	printf("key-value pair: %s %s\n", node.key, node.value);
	free(node.value);
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

	node.key = argv[0];
	node.key_size = strlen(argv[0]);
	
	ret = ioctl(fd, KO_TEST_IOCTL_DEL, &node);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_DEL");
		return -1;
	}
	printf("key-value pair %s deleted\n", node.key);
	return 0;
}

static int cmd_read(int fd, int argc, char **argv)
{
	ko_test_node node;
	int ret;

    	ret = ioctl(fd, KO_TEST_IOCTL_READ_BEGIN);
	if (ret < 0)
	{
		perror("ioctl - KO_TEST_IOCTL_READ_BEGIN");
		return -1;
	}

	int index = 0;
	for (;;)
	{
		memset(&node, 0, sizeof(node));
		ret = ioctl(fd, KO_TEST_IOCTL_READ_NEXT, &node);
		if (ret < 0)
		{
		    if (errno == ENOENT)
			break;
		    if (errno == ENOSPC)
		    {
			node.key = realloc_string(node.key, node.key_size);
			node.value = realloc_string(node.value, node.value_size);
			ret = ioctl(fd, KO_TEST_IOCTL_READ_NEXT, &node);
		    }
		    if (ret < 0)
		    {
			perror("ioctl - KO_TEST_IOCTL_READ_NEXT");
			break;
		    }
		    printf("#%d: %s - %s\n", index, node.key, node.value);
		    index++;
		}
	}	
	ret = ioctl(fd, KO_TEST_IOCTL_READ_END);
	if (ret < 0)
	{
		perror("ioctl - KO_TEST_IOCTL_READ_END");
		return -1;
	}
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

	if (argc > 1)
	{
		int res;
		const char *command = argv[1];
		argc -= 2;
		argv += 2;

		if (strcmp(command, "add") == 0)
			res = cmd_add(fd, argc, argv);
		else if (strcmp(command, "del") == 0)
			res = cmd_del(fd, argc, argv);
		else if (strcmp(command, "get") == 0)
			res = cmd_get(fd, argc, argv);
		else if (strcmp(command, "read") == 0)
			res = cmd_read(fd, argc, argv);
		else
			printf("unknown command: %s\n", command);
		if (res != 0)
			printf("%s command execution failed\n",command);
	}
	
	unsigned int node_count = 0;
	ret = ioctl(fd, KO_TEST_IOCTL_COUNT, &node_count);
	if (ret == -1)
	{
		perror("ioctl - KO_TEST_IOCTL_COUNT");
		return EXIT_FAILURE;
	}
	printf("current key-value pairs count: %d\n", node_count);

	close(fd);
	return EXIT_SUCCESS;
}
