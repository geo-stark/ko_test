#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "ko_test_ioctl.h"

#define DEVICE_NAME "ko_test_device"
#define CLASS_NAME  "ko_test_class"

#undef  pr_fmt
#define pr_fmt(fmt) DEVICE_NAME ": " fmt

#define DEFAULT_HASH_TABLE_SIZE 4096
static unsigned int hash_table_size = DEFAULT_HASH_TABLE_SIZE;

module_param(hash_table_size, uint, 0444);
MODULE_PARM_DESC(hash_table_size, "Min size of hash table");

static unsigned int item_count;
static struct class *self_class;
static struct device *self_device;
static int major_number;
static bool device_write_locked;
static DEFINE_MUTEX(data_lock);
static struct kobject *sysfs_root_dir;
static struct kobject *sysfs_items_dir;
static ssize_t item_show(struct kobject *kobj, struct kobj_attribute *attr,
						char *buf);
static ssize_t item_store(struct kobject *kobj, struct kobj_attribute *attr,
						const char *buf, size_t count);

struct file_data {
	bool locked;
	int bucket;
	struct ht_item *pos;
};

struct ht_item {
	struct hlist_node entry;
	struct kobj_attribute attr;

	int value_size;
	char *value;
	int key_size;
	char key[];
};

static unsigned int ht_array_size;
static struct hlist_head *ht_table;

#undef HASH_SIZE
#define HASH_SIZE(x) ht_array_size

// taken from http://www.cse.yorku.ca/~oz/hash.html
static unsigned long djb2n(const char *str, int size)
{
	unsigned long hash = 5381;
	int i;

	for (i = 0; i < size; i++)
		hash = ((hash << 5) + hash) + (unsigned char)str[i]; /* hash * 33 + c */
	return hash;
}

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

static struct ht_item *ht_find_item(const char *key, int size, unsigned long *hash_out)
{
	struct ht_item *item;
	unsigned long hash;

	hash = djb2n(key, size);
	if (hash_out != NULL)
		*hash_out = hash;

	hash_for_each_possible(ht_table, item, entry, hash) {
		if (item->key_size == size && memcmp(item->key, key, size) == 0)
			return item;
	}
	return NULL;
}
static bool validate_key(const ko_test_node *node)
{
	int i;

	for (i = 0; i < node->key_size; i++)
		if (!isprint(node->key[i]))
			return false;
	return true;
}

static bool copy_value(struct ht_item *dst, const char *value, int size)
{
	dst->value = krealloc(dst->value, size, GFP_KERNEL);
	if (dst->value == NULL)
		return false;
	memcpy(dst->value, value, size);
	dst->value_size = size;
	return true;
}

static int ht_add_item(const ko_test_node *node, bool allow_replace)
{
	struct ht_item *item;
	int res, total_size;
	unsigned long hash;

	if (device_write_locked)
		return -EAGAIN;

	item = ht_find_item(node->key, node->key_size, &hash);
	if (item != NULL) {
		if (!allow_replace)
			return -EEXIST;
		if (!copy_value(item, node->value, node->value_size))
			return -ENOMEM;
		return 0;
	}
	if (!validate_key(node))
		return -EINVAL;

	total_size = sizeof(struct ht_item) + node->key_size + 1;
	item = kzalloc(total_size, GFP_KERNEL);
	if (item == NULL)
		return -ENOMEM;

	if (!copy_value(item, node->value, node->value_size)) {
		kfree(item);
		return -ENOMEM;
	}
	memcpy(item->key, node->key, node->key_size);
	item->key_size = node->key_size;
	// key is stored null-terminated only for sysfs attr
	item->key[item->key_size] = 0;

	hash_add(ht_table, &item->entry, hash);

	item->attr.attr.mode = 0600;
	item->attr.attr.name = item->key;
	item->attr.store = item_store;
	item->attr.show = item_show;
	res = sysfs_create_file(sysfs_items_dir, &item->attr.attr);
	if (res != 0)
		pr_err("sysfs_create_file failed\n");

	item_count++;

	return 0;
}

static int ht_del_item(const char *key, int size)
{
	struct ht_item *item;

	if (device_write_locked)
		return -EAGAIN;

	item = ht_find_item(key, size, NULL);
	if (item == NULL)
		return -ENOENT;

	hash_del(&item->entry);
	sysfs_remove_file(sysfs_items_dir, &item->attr.attr);
	kfree(item->value);
	kfree(item);
	item_count--;
	return 0;
}

static void ht_del_items(void)
{
	struct ht_item *pos;
	struct hlist_node *tmp;
	unsigned int bkt;

	hash_for_each_safe(ht_table, bkt, tmp, pos, entry) {
		hash_del(&pos->entry);
		sysfs_remove_file(sysfs_items_dir, &pos->attr.attr);
		kfree(pos->value);
		kfree(pos);
	}
	item_count = 0;
}

static void ht_destroy(void)
{
	ht_del_items();
	kfree(ht_table);
	ht_table = NULL;
}

static int ht_get_deepest_collision(void)
{
	struct ht_item *pos;
	unsigned int bkt, cc, tcc = 0;

	for (bkt = 0; bkt < HASH_SIZE(ht_table); bkt++) {
		cc = 0;
		hlist_for_each_entry(pos, &ht_table[bkt], entry)
			cc++;
		if (tcc < cc)
			tcc = cc;
	}
	return tcc;
}

struct ht_item *ht_read_init(int *bkt_in, struct ht_item **item_in)
{
	int bkt;
	struct ht_item *item;

	hash_for_each(ht_table, bkt, item, entry) {
		*bkt_in = bkt;
		*item_in = item;
		return item;
	}
	return NULL;
}

struct ht_item *ht_read_next(int *bkt_in, struct ht_item **item_in)
{
	int bkt;
	struct ht_item *item;

	item = *item_in;
	hlist_for_each_entry_continue(item, entry) {
		*item_in = item;
		return item;
	}

	for (bkt = *bkt_in + 1; bkt < HASH_SIZE(ht_table); bkt++) {
		hlist_for_each_entry(item, &ht_table[bkt], entry) {
			*bkt_in = bkt;
			*item_in = item;
			return item;
		}
	}
	*bkt_in = bkt;
	*item_in = NULL;
	return NULL;
}

static ssize_t item_show(struct kobject *kobj, struct kobj_attribute *attr,
						char *buf)
{
	struct ht_item *item;
	ssize_t res = -ENOENT;

	mutex_lock(&data_lock);
	item = ht_find_item(attr->attr.name, strlen(attr->attr.name), NULL);
	if (item != NULL) {
		memcpy(buf, item->value, item->value_size);
		res = item->value_size;
	}
	mutex_unlock(&data_lock);

	return res;
}

static ssize_t item_store(struct kobject *kobj, struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	ssize_t res;
	ko_test_node node;

	node.key = (char*)attr->attr.name;
	node.key_size = strlen(attr->attr.name);
	node.value = (char*)buf;
	node.value_size = count;

	mutex_lock(&data_lock);
	if ((res = ht_add_item(&node, true)) == 0)
		res = count;
	mutex_unlock(&data_lock);
	return res;
}

static bool read_key_value(const char *buf, size_t count, ko_test_node *node)
{
	const char *end = buf + count;

	node->key = (char*)buf;
	while (buf != end)
	if (*buf++ == '\n') {
		node->key_size = buf - node->key - 1;
		node->value = (char*)buf;
		node->value_size = end - buf;
		return true;
	}
	return false;
}

static ssize_t add_set_store(struct kobject *kobj, struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	ssize_t res;
	bool allow_replace;
	ko_test_node node;

	if (!read_key_value(buf, count, &node))
		return -ENOENT;
	allow_replace = attr->attr.name[0] == 's';
	mutex_lock(&data_lock);
	if ((res = ht_add_item(&node, allow_replace)) == 0)
		res = count;
	mutex_unlock(&data_lock);
	return res;
}

static struct kobj_attribute set_attr = {
	.attr = {
		.name = "set",
		.mode = 0200
	},
	.store = add_set_store,
};

static struct kobj_attribute add_attr = {
	.attr = {
		.name = "add",
		.mode = 0200
	},
	.store = add_set_store,
};

static ssize_t delete_store(struct kobject *kobj, struct kobj_attribute *attr,
						 const char *buf, size_t count)
{
	ssize_t res = -ENOENT;

	mutex_lock(&data_lock);
	if (ht_del_item(buf, count) == 0)
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

static ssize_t locked_show(struct kobject *kobj, struct kobj_attribute *attr,
						char *buf)
{
	return sprintf(buf, "%s\n", device_write_locked ? "1" : "0");
}

static struct kobj_attribute locked_attr = {
	.attr = {
		.name = "locked",
		.mode = 0400
	},
	.show = locked_show,
};

static ssize_t collision_counter_show(struct kobject *kobj, 
		struct kobj_attribute *attr, char *buf)
{
	int counter;

	mutex_lock(&data_lock);
	counter = ht_get_deepest_collision();
	mutex_unlock(&data_lock);

	return sprintf(buf, "%d\n", counter);
}

static struct kobj_attribute collision_counter_attr = {
	.attr = {
		.name = "collision_counter",
		.mode = 0400
	},
	.show = collision_counter_show,
};

static struct kobj_attribute *sysfs_root_files[] = {
	&delete_attr,
	&add_attr,
	&set_attr,
	&locked_attr,
	&collision_counter_attr,
};

static int init_sysfs(void)
{
	int res, i;

	sysfs_root_dir = kobject_create_and_add("data", &self_device->kobj);
	if (!sysfs_root_dir)
		return -ENOMEM;

	sysfs_items_dir = kobject_create_and_add("items", sysfs_root_dir);
	if (!sysfs_items_dir)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(sysfs_root_files); i++) {
		res = sysfs_create_file(sysfs_root_dir, &sysfs_root_files[i]->attr);
		if (res != 0) {
			pr_err("sysfs_create_file for %s failed\n",
				sysfs_root_files[i]->attr.name);
			return res;
		}
	}
	return 0;
}

static void destroy_sysfs(void)
{
	int i;
	
	for (i = 0; i < ARRAY_SIZE(sysfs_root_files); i++)
		sysfs_remove_file(sysfs_root_dir, &sysfs_root_files[i]->attr);

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

static int load_key_value_user(ko_test_node *node, void __user *arg_user)
{
	char *ptr;

	if (copy_from_user(node, arg_user, sizeof(ko_test_node)) != 0)
		return -EFAULT;
	if (node->key_size <= 0 || node->key == NULL || node->value_size < 0)
		return -EINVAL;
	if ((ptr = kmalloc(node->key_size, GFP_KERNEL)) == NULL)
		return -ENOMEM;
	if (copy_from_user(ptr, node->key, node->key_size) != 0) {
		kfree(node->key);
		return -EFAULT;
	}
	node->key = ptr;

	if (node->value != NULL) {
		if ((ptr = kmalloc(node->value_size, GFP_KERNEL)) == NULL) {
			kfree(node->key);
			return -ENOMEM;
		}
		if (copy_from_user(ptr, node->value, node->value_size) != 0) {
			kfree(node->key);
			kfree(ptr);
			return -EFAULT;
		}
		node->value = ptr;
	}
	return 0;
}

static int load_key_user(char **key, const ko_test_node *node)
{
	char *ptr;

	if (node->key_size <= 0 || node->key == NULL || node->value_size < 0)
		return -EINVAL;
	if ((ptr = kmalloc(node->key_size, GFP_KERNEL)) == NULL)
		return -ENOMEM;
	if (copy_from_user(ptr, node->key, node->key_size) != 0) {
		kfree(ptr);
		return -EFAULT;
	}
	*key = ptr;
	return 0;
}

static long device_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long argp)
{
	struct file_data *fd;
	void __user *arg_user;
	int res = 0;

	fd = (struct file_data *)file->private_data;
	arg_user = (void __user *)argp;
	switch (cmd) {
	case KO_TEST_IOCTL_VERSION: {
		char data[KO_TEST_MAX_VERSION_SIZE];
		sprintf(data, "%s-%s", DEVICE_NAME, THIS_MODULE->version);
		if (copy_to_user(arg_user, data, strlen(data) + 1) != 0)
			return -EFAULT;
		return 0;
	}

	case KO_TEST_IOCTL_SET:
	case KO_TEST_IOCTL_ADD: {
		ko_test_node node;

		if ((res = load_key_value_user(&node, arg_user)) != 0)
			return res;
		mutex_lock(&data_lock);
		res = ht_add_item(&node, cmd == KO_TEST_IOCTL_SET);
		mutex_unlock(&data_lock);
		kfree(node.key);
		kfree(node.value);
		return res;
	}
	case KO_TEST_IOCTL_GET: {
		ko_test_node node;
		struct ht_item *item;
		char *key;

		if (copy_from_user(&node, arg_user, sizeof(ko_test_node)) != 0)
			return -EFAULT;
		if ((res = load_key_user(&key, &node)) != 0)
			return res;

		mutex_lock(&data_lock);
		item = ht_find_item(key, node.key_size, NULL);
		kfree(key);
		if (item == NULL)
			res = -ENOENT;
		else {
			if (node.value_size < item->value_size)
				res = -ENOSPC;
			else if (copy_to_user(node.value, item->value, item->value_size) != 0)
				res = -EFAULT;
			node.value_size = item->value_size;
		}
		mutex_unlock(&data_lock);
		if (copy_to_user(arg_user, &node, sizeof(node)) != 0)
			res = -EFAULT;
		return res;
	}
	case KO_TEST_IOCTL_DEL: {
		ko_test_node node;
		char *key;

		if (copy_from_user(&node, arg_user, sizeof(ko_test_node)) != 0)
			return -EFAULT;
		if ((res = load_key_user(&key, &node)) != 0)
			return res;

		mutex_lock(&data_lock);
		res = ht_del_item(key, node.key_size);
		mutex_unlock(&data_lock);
		return res;
	}
	case KO_TEST_IOCTL_COUNT: {
		int count = 0;
		mutex_lock(&data_lock);
		count = item_count;
		mutex_unlock(&data_lock);

		if (copy_to_user(arg_user, &count, sizeof(int)) != 0)
			return -EFAULT;
		return 0;
	}
	case KO_TEST_IOCTL_READ_BEGIN: {
		mutex_lock(&data_lock);
		if (device_write_locked)
			res = -EBUSY;
		else {
			device_write_locked = true;
			fd->locked = true;
			ht_read_init(&fd->bucket, &fd->pos);
		}
		mutex_unlock(&data_lock);
		return res;
	}
	case KO_TEST_IOCTL_READ_END: {
		mutex_lock(&data_lock);
		if (!fd->locked)
			res = -EBUSY;
		else {
			device_write_locked = false;
			fd->locked = false;
		}
		mutex_unlock(&data_lock);
		return res;
	}
	case KO_TEST_IOCTL_READ_NEXT: {
		ko_test_node node;

		if (copy_from_user(&node, arg_user, sizeof(ko_test_node)) != 0)
			return -EFAULT;

		mutex_lock(&data_lock);
		if (!fd->locked) {
			mutex_unlock(&data_lock);
			return -EBUSY;
		}
		if (fd->pos == NULL) {
			mutex_unlock(&data_lock);
			return -ENOENT;
		}

		if (node.value_size < fd->pos->value_size)
			res = -ENOSPC;
		if (node.key_size < fd->pos->key_size)
			res = -ENOSPC;

		if (res == 0) {
			if (copy_to_user(node.key, fd->pos->key, fd->pos->key_size) != 0)
				res = -EFAULT;
			if (copy_to_user(node.value, fd->pos->value, fd->pos->value_size) != 0)
				res = -EFAULT;
		}
		node.key_size = fd->pos->key_size;
		node.value_size = fd->pos->value_size;
		if (copy_to_user(arg_user, &node, sizeof(node)) != 0)
			res = -EFAULT;
		if (res == 0)
			ht_read_next(&fd->bucket, &fd->pos);
		mutex_unlock(&data_lock);
		return res;
	}
	default:
		return -EINVAL;
	}
	return 0;
}

static int device_open(struct inode *inode, struct file *file)
{
	struct file_data *fd;

	fd = kzalloc(sizeof(struct file_data), GFP_KERNEL);
	if (fd == NULL)
		return -ENOMEM;

	file->private_data = fd;
	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	struct file_data *fd;

	fd = (struct file_data *)file->private_data;
	if (fd->locked) {
		mutex_lock(&data_lock);
		device_write_locked = false;
		mutex_unlock(&data_lock);
	}
	kfree(fd);
	return 0;
}

static int __init ko_test_init(void)
{
	int res = 0;
	
	pr_info("started\n");
	major_number = register_chrdev(0, DEVICE_NAME, &file_ops);
	if (major_number < 0) {
		pr_err("failed to register a major number\n");
		return major_number;
	}

	// Register the device class
	self_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR_OR_NULL(self_class)) {
		// Check for error and clean up if there is
		unregister_chrdev(major_number, DEVICE_NAME);
		pr_err("failed to register device class\n");
		return -ENOMEM;
	}
	// Register the device driver
	self_device = device_create(self_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR_OR_NULL(self_device)) {
		class_destroy(self_class);
		unregister_chrdev(major_number, DEVICE_NAME);
		pr_err("failed to create the device\n");
		return -ENOMEM;
	}

	res = ht_init();
	if (res < 0) {
		device_destroy(self_class, MKDEV(major_number, 0));
		class_destroy(self_class);
		unregister_chrdev(major_number, DEVICE_NAME);
		pr_err("failed to create hash table\n");
		return res; 
	}
	mutex_init(&data_lock);
	res = init_sysfs();
	if (res < 0) {
		mutex_destroy(&data_lock);
		ht_destroy();
		device_destroy(self_class, MKDEV(major_number, 0));
		class_destroy(self_class);
		unregister_chrdev(major_number, DEVICE_NAME);
		pr_err("failed to create hash table\n");
		return res; 
	}
	pr_info("hash table size, specified %u, real %u\n", 
		hash_table_size, ht_array_size);
	return 0;
}

static void __exit ko_test_exit(void)
{
	ht_destroy();
	destroy_sysfs();
	mutex_destroy(&data_lock);
	device_destroy(self_class, MKDEV(major_number, 0));
	class_destroy(self_class);
	unregister_chrdev(major_number, DEVICE_NAME);
	pr_info("stopped\n");
}

module_init(ko_test_init);
module_exit(ko_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("George Stark");
MODULE_DESCRIPTION("Test to get a job)");
MODULE_VERSION("0.3");

// TODO:
// try RCU
// support block / nonblock mode, when try to change locked data
// add collission counter