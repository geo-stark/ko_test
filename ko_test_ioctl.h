#ifndef KO_TEST_IOCTL_H
#define KO_TEST_IOCTL_H

#include <linux/ioctl.h>

typedef struct
{
	int key;
	int value;
} ko_test_node;

#define KO_TEST_MAX_VERSION_SIZE  128

#define KO_TEST_IOCTL_MAGIC      'T'
#define KO_TEST_IOCTL_VERSION    _IOR(KO_TEST_IOCTL_MAGIC, 0, char *)
#define KO_TEST_IOCTL_ADD        _IOW(KO_TEST_IOCTL_MAGIC, 1, ko_test_node *)
#define KO_TEST_IOCTL_SET        _IOW(KO_TEST_IOCTL_MAGIC, 2, ko_test_node *)
#define KO_TEST_IOCTL_GET        _IOWR(KO_TEST_IOCTL_MAGIC, 3, ko_test_node *)
#define KO_TEST_IOCTL_DEL        _IOW(KO_TEST_IOCTL_MAGIC, 4, ko_test_node *)
#define KO_TEST_IOCTL_COUNT      _IOR(KO_TEST_IOCTL_MAGIC, 5, unsigned int *)

#define KO_TEST_IOCTL_READ_BEGIN _IO(KO_TEST_IOCTL_MAGIC, 6)
#define KO_TEST_IOCTL_READ_NEXT  _IOR(KO_TEST_IOCTL_MAGIC, 7, ko_test_node *)
#define KO_TEST_IOCTL_READ_END   _IO(KO_TEST_IOCTL_MAGIC, 8)

#endif // KO_TEST_IOCTL_H
