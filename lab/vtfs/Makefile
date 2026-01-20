obj-m += vtfs.o
vtfs-objs := source/vtfs.o source/http.o

PWD := $(CURDIR)
KDIR = /lib/modules/`uname -r`/build
EXTRA_CFLAGS = -Wall -g

all:
	make -C $(KDIR) M=$(PWD) modules
	cp -f vtfs.ko source/vtfs.ko

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -rf .cache
	rm -f source/vtfs.ko
