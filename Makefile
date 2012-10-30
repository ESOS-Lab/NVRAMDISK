# Comment/uncomment the following line to disable/enable debugging
#DEBUG = y


# Add your debugging flag (or not) to CFLAGS
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSBULL_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS)
#EXTRA_CFLAGS += I..

ifneq ($(KERNELRELEASE),)
# call from kernel build system

obj-m	:= nvdisk.o

else

KERNELDIR ?= /lib/modules/2.6.36.zone/build
PWD       := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif



clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions

depend .depend dep:
	$(CC) $(EXTRA_CFLAGS) -M -include include/linux/autoconf.h *.c > .depend


ifeq (.depend,$(wildcard .depend))
include .depend
endif
