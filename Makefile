ifneq ($(KERNELRELEASE),)
	obj-m := driver.o

else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	g++ -o user_app user_level_program.cpp

endif