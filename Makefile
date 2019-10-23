obj-m += ko_test.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
	cp *.ko /nfs/ko > /dev/null
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean