MODULE_NAME := bmp280-iio
SRC_DIR := src
$(MODULE_NAME)-y := $(SRC_DIR)/main.o $(SRC_DIR)/bmp280-iio.o $(SRC_DIR)/bmp280.o
obj-m += $(MODULE_NAME).o

KERNEL_VERSION := $(shell uname -r)

all: dtbo modules

dtbo: $(MODULE_NAME).dts
	dtc -@ -I dts -O dtb -o $(MODULE_NAME).dtbo $(MODULE_NAME).dts
	echo "Built Device Tree Overlay"
modules:
	make -C /usr/lib/modules/$(KERNEL_VERSION)/build M=$(CURDIR) modules
	echo "Built Kernel Module"
modules_install:
	make -C /usr/lib/modules/$(KERNEL_VERSION)/build M=$(CURDIR) modules_install
	echo "Installed Kernel Module"
clean:
	rm $(MODULE_NAME).dtbo
	make -C /usr/lib/modules/$(KERNEL_VERSION)/build M=$(CURDIR) clean
