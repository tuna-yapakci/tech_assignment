#ifneq ($(KERNELRELEASE),)
#	obj-m := driver.o

#else
#	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
#	PWD := $(shell pwd)

#default:
#	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
#	g++ -Wall -o user_app user_level_program.cpp

#endif

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	obj-m := driver.o
	$(MAKE) -C $(KERNELDIR) M=$(PWD)/master modules
	obj-m := driver2.o
	$(MAKE) -C $(KERNELDIR) M=$(PWD)/slave modules
	g++ -Wall -o user_app user_level_program.cpp