obj-m += gpd-fan.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all default: modules
install: modules_install

modules modules_install help clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(shell pwd) $@