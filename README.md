# dadi_vdb
dadi block device driver mode


Usage,


make KDOOT=linux_kernel_dir 
or just make, to use host kernel build environment

sudo insmod ./vdb.ko

By default, overlay_vdb uses /test.lsmtz file as file backend, but it can be configured as parameter

sudo insmod ./vdo.ko backfile=/tmp/layer0.lsmtz


after that,

mknod /dev/ovdb b 231 1
mount /dev/ovdb /mnt


