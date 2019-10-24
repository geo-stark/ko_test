#ifndef KO_TEST_IOCTL_H
#define KO_TEST_IOCTL_H

#include <linux/ioctl.h>

// This type is used for almost data exchange beetwen kernel and user environment
// key and value are ascii strings. Both string are  not null-terminated.
// Key should not contain characters illegal for *nix filesystem or it won't be published 
// in sysfs
// key_max_size and value_max_size contain buffers size for read / write
// if field not used / need it must be NULL

typedef struct
{
	char *key;
	int key_size;
	char *value;
	int value_size;
} ko_test_node;

#define KO_TEST_MAX_VERSION_SIZE  128

#define KO_TEST_IOCTL_MAGIC      'S'
#define KO_TEST_IOCTL_VERSION    _IOR(KO_TEST_IOCTL_MAGIC, 0, char *)
#define KO_TEST_IOCTL_ADD        _IOW(KO_TEST_IOCTL_MAGIC, 1, ko_test_node *)
#define KO_TEST_IOCTL_SET        _IOW(KO_TEST_IOCTL_MAGIC, 2, ko_test_node *)
#define KO_TEST_IOCTL_GET        _IOWR(KO_TEST_IOCTL_MAGIC, 3, ko_test_node *)
#define KO_TEST_IOCTL_DEL        _IOW(KO_TEST_IOCTL_MAGIC, 4, ko_test_node *)
#define KO_TEST_IOCTL_COUNT      _IOR(KO_TEST_IOCTL_MAGIC, 5, unsigned int *)

#define KO_TEST_IOCTL_READ_BEGIN _IO(KO_TEST_IOCTL_MAGIC, 6)
#define KO_TEST_IOCTL_READ_NEXT  _IOWR(KO_TEST_IOCTL_MAGIC, 7, ko_test_node *)
#define KO_TEST_IOCTL_READ_END   _IO(KO_TEST_IOCTL_MAGIC, 8)

// if ioctl returns ENOSPC, key_size and value size contain required buffer sizes

#endif // KO_TEST_IOCTL_H
