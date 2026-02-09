obj-m += mvvmm_guest.o

KDIR := ../$(shell ls ../ | grep linux-)/

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean