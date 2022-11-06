$(shell cp driver.c driver2.c)

obj-m += driver.o
obj-m += driver2.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	g++ -Wall -o user_app user_level_program.cpp

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean