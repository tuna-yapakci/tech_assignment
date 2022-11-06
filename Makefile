KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += driver.o
obj-m += driver2.o

all:
	$(shell cp driver.c driver2.c)
	$(shell chmod +x loader.sh)
	$(shell chmod +x remover.sh)
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	g++ -Wall -o user_app user_level_program.cpp
	rm *.mod*
	rm *.o
	rm .*.cmd
	rm *odule*

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean