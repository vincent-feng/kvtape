kvtape_module-objs := kvtape.o kernel_fop.o
obj-m += kvtape_module.o
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm *.ko
	rm *.o