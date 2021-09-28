# dadi_vbd
dadi block device driver mode

Usage,

make KROOT=linux_kernel_dir 
or just make, to use host kernel build environment

sudo insmod ./vbd.ko

By default, overlay_vdb uses /test.lsmtz file as file backend, but it can be configured as parameter

sudo insmod ./vbd.ko backfile=/tmp/layer0.lsmtz


after that,

mount /dev/vbd0 /mnt


