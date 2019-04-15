rmmod simplefs 
insmod simplefs.ko 
dd if=/dev/zero of=./image bs=4096 count=1024
../simplefs/mkfs-simplefs ./image
hexdump -C ./image > /tmp/a.txt
mount -o loop -t simplefs ./image /mnt/simplefs
touch /mnt/simplefs/abc
rm /mnt/simplefs/abc
umount /mnt/simplefs 
hexdump -C ./image > /tmp/c.txt
diff -uprN /tmp/a.txt /tmp/c.txt 
