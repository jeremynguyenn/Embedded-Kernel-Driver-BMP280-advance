/**
 * This file is the main entry point for the module.
 * It sets up an I2C driver for the BMP280 as a kernel module. On the probe
 * method, it exposes an IIO device.
 * This file contains all the module definition and I2C driver code. The logic
 * for IIO support is in `bmp280-iio.c`, while the logic for talking with the
 * BMP280 sensor is in `bmp280.c`.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/stat.h>

#include "bmp280.h"

MODULE_AUTHOR("Leonardo Blanger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("An IIO compatible, I2C driver for the Bosch BMP280 "
		   "temperature and pressure sensor.");

/**
 * The industrialio and industrialio-triggered-buffer kernel modules are hard
 * dependencies.
 */
MODULE_SOFTDEP("pre: industrialio industrialio-triggered-buffer");

/**
 * Expected I2C address. Can be configured as a module parameter if your sensor
 * somehow has a different address.
 * E.g. `sudo insmod bmp280-iio.ko bmp280_i2c_address=<addr>`
 */
static unsigned short bmp280_i2c_address = 0x76;
module_param(bmp280_i2c_address, ushort, S_IRUGO);
MODULE_PARM_DESC(bmp280_i2c_address, "I2C address for the BMP280 sensor");

/**
 * Traditional device table matching approach.
 * Listed here for completness only, since we rely mostly on the device tree.
 */
static const struct i2c_device_id bmp280_iio_i2c_driver_ids[] = {
  {
    .name = "leonardo,bmp280-iio",
  },
  { /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, bmp280_iio_i2c_driver_ids);

/**
 * Device Tree (OF = open firmware) based matching ids.
 */
static const struct of_device_id bmp280_iio_of_driver_ids[] = {
  {
    .compatible = "leonardo,bmp280-iio",
  },
  { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bmp280_iio_of_driver_ids);

static int bmp280_iio_probe(struct i2c_client *client);
static void bmp280_iio_remove(struct i2c_client *client);

static struct i2c_driver bmp280_iio_driver = {
  .probe = bmp280_iio_probe,
  .remove = bmp280_iio_remove,
  .id_table = bmp280_iio_i2c_driver_ids,
  .driver = {
    .name = "leonardo,bmp280-iio",
    .of_match_table = of_match_ptr(bmp280_iio_of_driver_ids),
  },
};

/**
 * I2C driver probe.
 * Performs a sanity check on the client address, then calls up initialization
 * and registration with the IIO subsystem.
 */
static int bmp280_iio_probe(struct i2c_client *client) {
  pr_info("Probing the i2c driver.\n");
  if (client->addr != bmp280_i2c_address) {
    pr_err("Probed with unexpected I2C address 0x%02x. Expecting 0x%02x\n",
	   client->addr, bmp280_i2c_address);
    return -1;
  }
  int status = register_bmp280_iio_device(client);
  if (status) {
    return status;
  }
  pr_info("Probed i2c driver successfully.\n");
  return 0;
}

/**
 * I2C driver remove.
 * We do not need to undo anything manually here.
 * `register_bmp280_iio_device` is written to use only "device managed"
 * operations, (i.e. devm_*() functions). Those actions are automatically undone
 * when client->dev is removed.
 */
static void bmp280_iio_remove(struct i2c_client *client) {
  pr_info("Removing the i2c driver.\n");
}

/**
 * This macro gets replaced by a module __init/__exit function pair,
 * that does nothing other than registering/unregistering the I2C driver.
*/
module_i2c_driver(bmp280_iio_driver);
