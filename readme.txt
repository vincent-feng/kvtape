Emulate a tape device on Linux kernel space.

Tested on kernel 2.6.32.

To use this module, please follow the steps:
(1)Use dd to Make a file named vtape.dat under /home directory. For example,
dd if=/dev/zero of=/home/vtape.dat bs=1M count=100
(2)make
(3)insmod kvtape_module.ko
(4)Then tape device files /dev/st* appear.
 
