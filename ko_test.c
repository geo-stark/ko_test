#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "ko_test_ioctl.h"

#define DEVICE_NAME "ko_test_device"
#define CLASS_NAME  "ko_test_class"

#define DEFAULT_HASH_TABLE_SIZE 4096
static unsigned int hash_table_size = DEFAULT_HASH_TABLE_SIZE;

module_param(hash_table_size, uint, 0600);
MODULE_PARM_DESC(hash_table_size, "Max size of hash table");

static unsigned int item_count;
static struct class *self_class;
static struct device *self_device;
static int major_number;
static int device_open_count;
static DEFINE_MUTEX(data_lock);
static struct kobject *sysfs_root_dir;
static struct kobject *sysfs_items_dir;
static ssize_t item_show(struct kobject *kobj, struct kobj_attribute *attr,
						 char *buf);
static ssize_t item_store(struct kobject *kobj, struct kobj_attribute *attr,
						  const char *buf, size_t count);

struct ht_item {
	int key;
	int value;

	char key_str[32];
	struct kobj_attribute attr;

	struct hlist_node entry;
};

static unsigned int ht_array_size;
static struct hlist_head *ht_table;

#undef HASH_SIZE
#define HASH_SIZE(x) ht_array_size

static int ht_init(void)
{
	unsigned int i;
	
	ht_array_size = 1 << fls(hash_table_size);
	ht_table = kcalloc(sizeof(struct ht_item), ht_array_size, GFP_KERNEL);
	if (ht_table == NULL)
		return -ENOMEM;
	for (i = 0; i < ht_array_size; i++)
		INIT_HLIST_HEAD(&ht_table[i]);
	return 0;
}

static void ht_destroy(void)
{
	kfree(ht_table);
}

static struct ht_item *ht_find_item(int key)
{
	struct ht_item *item;
	hash_for_each_possible(ht_table, item, entry, key) {
		if (item->key == key)
			return item;
	}
	return NULL;
}

static int ht_add_item(const ko_test_node *node)
{
	struct ht_item *item;
	int res;

	item = ht_find_item(node->key);
	if (item != NULL)
		return -EEXIST;

	item = kzalloc(sizeof(struct ht_item), GFP_KERNEL);
	if (item == NULL)
		return -ENOMEM;
	item->key = node->key;
	item->value = node->value;

	hash_add(ht_table, &item->entry, node->key);

	sprintf(item->key_str, "%d", item->key);
	item->attr.attr.mode = 0600;
	item->attr.attr.name = item->key_str;
	item->attr.store = item_store;
	item->attr.show = item_show;
	res = sysfs_create_file(sysfs_items_dir, &item->attr.attr);
	if (res != 0)
		printk(DEVICE_NAME ": sysfs_create_file failed\n");

	item_count++;

	return 0;
}

static int ht_del_item(int key)
{
	struct ht_item *item;

	item = ht_find_item(key);
	if (item == NULL)
		return -ENOENT;

	hash_del(&item->entry);

	sysfs_remove_file(sysfs_items_dir, &item->attr.attr);

	kfree(item);
	item_count--;
	return 0;
}

static void ht_del_items(void)
{
	struct ht_item *pos;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(ht_table, bkt, tmp, pos, entry)
		hash_del(&pos->entry);
}

static ssize_t item_show(struct kobject *kobj, struct kobj_attribute *attr,
						 char *buf)
{
	struct ht_item *item;
	ssize_t res = -ENOENT;
	int key;

	key = simple_strtol(attr->attr.name, NULL, 10);
	mutex_lock(&data_lock);
	item = ht_find_item(key);
	if (item != NULL)
		res = sprintf(buf, "%d\n", item->value);
	mutex_unlock(&data_lock);

	return res;
}

static ssize_t item_store(struct kobject *kobj, struct kobj_attribute *attr,
						  const char *buf, size_t count)
{
	struct ht_item *item;
	ssize_t res = -ENOENT;
	int key;

	key = simple_strtol(attr->attr.name, NULL, 10);
	mutex_lock(&data_lock);
	item = ht_find_item(key);
	if (item != NULL && sscanf(buf, "%d", &item->value) == 1)
		res = count;
	mutex_unlock(&data_lock);
	return res;
}

static ssize_t set_store(struct kobject *kobj, struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	struct ht_item *item;
	ssize_t res = -ENOENT;
	ko_test_node node;

	if (sscanf(buf, "%d,%d", &node.key, &node.value) != 2)
		return res;

	mutex_lock(&data_lock);
	item = ht_find_item(node.key);
	if (item == NULL) {
		if (ht_add_item(&node) == 0)
			res = count;
	}
	else {
		item->value = node.value;
		res = count;
	}
	
	mutex_unlock(&data_lock);
	return res;
}

static struct kobj_attribute set_attr = {
	.attr = {
		.name = "set",
		.mode = 0200
	},
	.store = set_store,
};

static ssize_t add_store(struct kobject *kobj, struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	ssize_t res = -ENOENT;
	ko_test_node node;

	if (sscanf(buf, "%d,%d", &node.key, &node.value) != 2)
		return res;

	mutex_lock(&data_lock);
	if (ht_add_item(&node) == 0)
		res = count;
	mutex_unlock(&data_lock);
	return res;
}

static struct kobj_attribute add_attr = {
	.attr = {
		.name = "add",
		.mode = 0200
	},
	.store = add_store,
};

static ssize_t delete_store(struct kobject *kobj, struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	ssize_t res = -ENOENT;
	int key;

	if (sscanf(buf, "%d", &key) != 1)
		return res;

	mutex_lock(&data_lock);
	if (ht_del_item(key) == 0)
		res = count;
	mutex_unlock(&data_lock);
	return res;
}

static struct kobj_attribute delete_attr = {
	.attr = {
		.name = "delete",
		.mode = 0200
	},
	.store = delete_store,
};

static int init_sysfs(void)
{
	int res;

	sysfs_root_dir = kobject_create_and_add("data", &self_device->kobj);
	if (!sysfs_root_dir)
		return -ENOMEM;

	sysfs_items_dir = kobject_create_and_add("items", sysfs_root_dir);
	if (!sysfs_items_dir)
		return -ENOMEM;

	res = sysfs_create_file(sysfs_root_dir, &delete_attr.attr);
	if (res != 0)
		printk(DEVICE_NAME ": sysfs_create_file(delete_attr) failed\n");
	res = sysfs_create_file(sysfs_root_dir, &add_attr.attr);
	if (res != 0)
		printk(DEVICE_NAME ": sysfs_create_file(add_attr) failed\n");
	res = sysfs_create_file(sysfs_root_dir, &set_attr.attr);
	if (res != 0)
		printk(DEVICE_NAME ": sysfs_create_file(set_attr) failed\n");
	return 0;
}

static void destroy_sysfs(void)
{
	kobject_put(sysfs_items_dir);
	kobject_put(sysfs_root_dir);
}

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static long device_unlocked_ioctl(struct file *, unsigned int, unsigned long);

static struct file_operations file_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = device_unlocked_ioctl,
	.open = device_open,
	.release = device_release
};

static long device_unlocked_ioctl(struct file *fl, unsigned int cmd, unsigned long argp)
{
	void __user *arg_user;
	int res;

	arg_user = (void __user *)argp;
	switch (cmd) {
	case KO_TEST_IOCTL_VERSION:	{
		char data[KO_TEST_MAX_VERSION_SIZE];
		sprintf(data, "%s-%s", DEVICE_NAME, THIS_MODULE->version);
		if (copy_to_user(arg_user, data, strlen(data) + 1))
			return -EFAULT;
		break;
	}
	case KO_TEST_IOCTL_ADD:	{
		ko_test_node node;

		if (copy_from_user(&node, arg_user, sizeof(node)))
			return -EFAULT;

		mutex_lock(&data_lock);
		res = ht_add_item(&node);
		mutex_unlock(&data_lock);
		return res;
	}
	case KO_TEST_IOCTL_GET:	{
		ko_test_node node;
		struct ht_item *item;

		if (copy_from_user(&node, arg_user, sizeof(node)))
			return -EFAULT;

		mutex_lock(&data_lock);
		item = ht_find_item(node.key);
		if (item != NULL) {
			node.key = item->key;
			node.value = item->value;
		}
		mutex_unlock(&data_lock);
		if (item != NULL) {
			if (copy_to_user(arg_user, &node, sizeof(node)))
				return -EFAULT;
			return 0;
		}
		return -ENOENT;
	}
	case KO_TEST_IOCTL_DEL: {
		ko_test_node node;
		int res;

		if (copy_from_user(&node, arg_user, sizeof(node)))
			return -EFAULT;

		mutex_lock(&data_lock);
		res = ht_del_item(node.key);
		mutex_unlock(&data_lock);
		return res;
	}
	break;
	case KO_TEST_IOCTL_COUNT: {
		int count = 0;
		mutex_lock(&data_lock);
		count = item_count;
		mutex_unlock(&data_lock);

		if (copy_to_user(arg_user, &count, sizeof(int)))
			return -EFAULT;
		break;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static int device_open(struct inode *inode, struct file *file)
{
	if (device_open_count)
		return -EBUSY;
	device_open_count++;
	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	device_open_count--;
	return 0;
}

static int __init ko_test_init(void)
{
	printk(DEVICE_NAME " started, hash table size: %u\n", hash_table_size);

	major_number = register_chrdev(0, DEVICE_NAME, &file_ops);
	if (major_number < 0) {
		printk(DEVICE_NAME ": failed to register a major number\n");
		return major_number;
	}
	printk(DEVICE_NAME ": registered correctly with major number %d\n", major_number);

	// Register the device class
	self_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(self_class)) {
		// Check for error and clean up if there is
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(DEVICE_NAME ": failed to register device class\n");
		return 0;
	}
	printk(DEVICE_NAME ": device class registered correctly\n");

	// Register the device driver
	self_device = device_create(self_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR(self_device)) {
		class_destroy(self_class);
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(DEVICE_NAME ": failed to create the device\n");
		return 0;
	}

	ht_init();
	mutex_init(&data_lock);
	init_sysfs();
	return 0;
}

static void __exit ko_test_exit(void)
{
	destroy_sysfs();
	mutex_destroy(&data_lock);
	ht_del_items();
	ht_destroy();
	device_destroy(self_class, MKDEV(major_number, 0));
	class_destroy(self_class);
	unregister_chrdev(major_number, DEVICE_NAME);
	printk(DEVICE_NAME " stopped\n");
}

module_init(ko_test_init);
module_exit(ko_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("George Stark");
MODULE_DESCRIPTION("Test to get a job)");
MODULE_VERSION("0.1");

// TODO:
// better error handling dureing module load
// move from printk to pr_* / debugfs
// move to string key /value
// required Kernel 3.7+ at least
