CONFIG_MODULE_SIG=n

MYPROC=vbd
obj-m += vbd.o 
vbd-objs := overlay_vbd.o zfile.o

export KROOT=/lib/modules/$(shell uname -r)/build
#export KROOT=/mnt/linux-bcache
#export KROOT=/lib/modules/$(uname)3.2.0-23-generic/build

allofit:  modules

modules: clean

	@$(MAKE) -C $(KROOT) M=$(PWD) modules

modules_install:
	@$(MAKE) -C $(KROOT) M=$(PWD) modules_install

kernel_clean:
	@$(MAKE) -C $(KROOT) M=$(PWD) clean

clean: kernel_clean
	rm -rf   Module.symvers modules.order

insert: modules
	sudo dmesg -c
	sudo insmod mybugondriver.ko

remove: clean
	sudo rmmod mybugondriver
