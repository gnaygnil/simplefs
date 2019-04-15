obj-m := simplefs.o
simplefs-objs := inode.o dir.o file.o
SRC = /lib/modules/$(shell uname -r)/build

all: ko mkfs-simplefs

ko:
	make -C $(SRC) M=$(PWD) modules


mkfs-simplefs_SOURCES:
	mkfs-simplefs.c simple_fs.h

clean:
	make -C $(SRC) M=$(PWD) clean
	rm mkfs-simplefs

