#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/sprintf.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

MODULE_AUTHOR("Leonardo Blanger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BMP280 live monitoring module "
		   "using the HD44780 character LCD display.");

/**
 * The bmp280-iio and hd44780 modules are dependencies,
 * and need to be loaded before.
 */
MODULE_SOFTDEP("pre: bmp280-iio hd44780");

/**
 * Monitor context structure.
 *
 * There is one instance of this allocated for each probed driver.
 *
 * Must be initialized before being used, and de-initialized after no longer
 * needed, through calls to monitor_init and monitor_teardown, respectively.
 */
struct bmp280_hd44780_monitor {
  struct mutex monitor_mutex;
  // References to BMP280 IIO channels
  struct iio_channel *temperature_channel;
  struct iio_channel *pressure_channel;
  // Work structure for the periodic data refresh
  struct delayed_work dwork;
  // ID of display we are writing to. Default to 0.
  s32 display_index;
  // How often do we update the display with new values. Default to 2 seconds.
  u32 refresh_period_ms;
  // Whether we are running or not. Default to true.
  bool running;
};

/**
 * Forward declarations of symbols from the hd44780 module.
 */
struct hd44780;
extern struct hd44780 *hd44780_get(int index);
extern void hd44780_put(struct hd44780 *hd44780);
extern int hd44780_reset_display(struct hd44780 *hd44780);
extern ssize_t hd44780_write(struct hd44780 *hd44780, const char *msg, size_t length);

static void bmp280_hd44780_monitor_work(struct work_struct *work);

/**
 * Initializes a monitor context structure.
 *
 * A call to this function must eventually be followed by a call to
 * monitor_teardown.
 *
 * Initializes the structure mutex, the monitor worker, and assign default
 * values to monitor parameters.
 */
static void monitor_init(struct bmp280_hd44780_monitor *monitor) {
  mutex_init(&monitor->monitor_mutex);
  // Set up workqueue entry for our running worker function
  INIT_DELAYED_WORK(&monitor->dwork, &bmp280_hd44780_monitor_work);
  // Assign default parameter values
  monitor->display_index = 0;
  monitor->refresh_period_ms = 2000;
  monitor->running = true;
}

/**
 * Monitor context structure teardown.
 *
 * Counterpart to monitor_init. Cancels (synchronously) the monitor worker, and
 * destroys the mutex.
 */
static void monitor_teardown(struct bmp280_hd44780_monitor *monitor) {
  // In case the worker is still running, make it stop.
  mutex_lock(&monitor->monitor_mutex);
  monitor->running = false;
  mutex_unlock(&monitor->monitor_mutex);
  // In case the worker is still scheduled, cancel it.
  cancel_delayed_work_sync(&monitor->dwork);
  mutex_destroy(&monitor->monitor_mutex);
}

/**
 * Monitor worker function.
 *
 * This is where the bulk of the work takes place. This function is responsible
 * for reading and parsing the temperature and pressure values from the BMP280
 * IIO channels, formatting them into human readable messages, retrieving the
 * required hd44780 display instance, and writing the messages to the display.
 *
 * This function gets scheduled to run periodically, according to the running
 * and refresh_period_ms parameters.
 */
static void bmp280_hd44780_monitor_work(struct work_struct *work) {
  struct delayed_work *dwork = container_of(work, struct delayed_work, work);
  struct bmp280_hd44780_monitor *monitor =
    container_of(dwork, struct bmp280_hd44780_monitor, dwork);
  mutex_lock(&monitor->monitor_mutex);
  int status = 0;
  // Read temperature from the BMP280 IIO channel
  int temperature = 0, temperature_val2 = 0;
  status = iio_read_channel_attribute(monitor->temperature_channel,
				      &temperature, &temperature_val2,
				      IIO_CHAN_INFO_PROCESSED);
  if (status < 0) {
    pr_err("Failed to read temperature value from IIO channel: %d\n", status);
    goto out;
  }
  if (status != IIO_VAL_FRACTIONAL) {
    pr_err("Unexpected IIO temperature channel type: %d\n", status);
    goto out;
  }
  // Read pressure from the BMP280 IIO channel
  int pressure = 0, pressure_val2 = 0;
  status = iio_read_channel_attribute(monitor->pressure_channel,
				      &pressure, &pressure_val2,
				      IIO_CHAN_INFO_PROCESSED);
  if (status < 0) {
    pr_err("Failed to read pressure value from IIO channel: %d\n", status);
    goto out;
  }
  if (status != IIO_VAL_FRACTIONAL) {
    pr_err("Unexpected IIO pressure channel type: %d\n", status);
    goto out;
  }
  // Compute integer and decimal parts
  int temperature_int = temperature / temperature_val2;
  int temperature_100ths =
    (100 * (temperature % temperature_val2)) / temperature_val2;
  // For pressure, use only integer part,
  // since the number in hPa is already long
  pressure_val2 *= 100; // Convert from Pascal to hecto-Pascal
  int pressure_int = pressure / pressure_val2;
  // Compose formatted string messages
  char temperature_msg[16];
  size_t temperature_msg_len =
    snprintf(temperature_msg, 16, "Temp: %3d.%02d C",
	     temperature_int, temperature_100ths);
  char pressure_msg[16];
  size_t pressure_msg_len =
    snprintf(pressure_msg, 16, "Pres: %4d hPa", pressure_int);
  // Retrieve the registered display, identified by display_index
  struct hd44780 *display = hd44780_get(monitor->display_index);
  if (IS_ERR(display)) {
    pr_err("Failed to retrieve display with index %d: %ld\n",
	   monitor->display_index, PTR_ERR(display));
    goto out;
  }
  // Clear the display before writing anything
  hd44780_reset_display(display);
  // Write the temperature message to the display
  hd44780_write(display, temperature_msg, temperature_msg_len);
  // Line break between the two messages
  hd44780_write(display, "\n", 1);
  // Write the pressure message to the display
  hd44780_write(display, pressure_msg, pressure_msg_len);
  // Release the display
  hd44780_put(display);
  display = NULL;
 out:
  // If we are still running, re-schedule the worker to run again after
  // refresh_period_ms milliseconds.
  if (monitor->running) {
    unsigned long delay = msecs_to_jiffies(monitor->refresh_period_ms);
    if (!schedule_delayed_work(dwork, delay)) {
      pr_err("Failed to reschedule worker thread.\n");
      monitor->running = false;
    }
  }
  mutex_unlock(&monitor->monitor_mutex);
}

static ssize_t
bmp280_hd44780_monitor_parameter_show(struct device *dev,
				      struct device_attribute *attr, char *buf);
static ssize_t
bmp280_hd44780_monitor_parameter_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count);

/**
 * Sysfs device attribute files for runtime configuration.
 */
static DEVICE_ATTR(monitor_display_index, 0644,
		   bmp280_hd44780_monitor_parameter_show,
		   bmp280_hd44780_monitor_parameter_store);
static DEVICE_ATTR(monitor_refresh_period_ms, 0644,
		   bmp280_hd44780_monitor_parameter_show,
		   bmp280_hd44780_monitor_parameter_store);
static DEVICE_ATTR(monitor_running, 0644,
		   bmp280_hd44780_monitor_parameter_show,
		   bmp280_hd44780_monitor_parameter_store);

/**
 * Sysfs device attribute show function.
 */
static ssize_t
bmp280_hd44780_monitor_parameter_show(struct device *dev,
				      struct device_attribute *attr, char *buf) {
  struct bmp280_hd44780_monitor *monitor = dev_get_drvdata(dev);
  if (mutex_lock_interruptible(&monitor->monitor_mutex)) {
    return -ERESTARTSYS;
  }
  ssize_t ret = 0;
  if (attr == &dev_attr_monitor_display_index) {
    ret = snprintf(buf, 12, "%d", monitor->display_index);
  } else if (attr == &dev_attr_monitor_refresh_period_ms) {
    ret = snprintf(buf, 11, "%u", monitor->refresh_period_ms);
  } else if (attr == &dev_attr_monitor_running) {
    ret = snprintf(buf, 2, "%d", monitor->running ? 1 : 0);
  } else {
    ret = -EINVAL;
  }
  mutex_unlock(&monitor->monitor_mutex);
  return ret;
}

/**
 * Sysfs device attribute store function.
 */
static ssize_t
bmp280_hd44780_monitor_parameter_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count) {
  // 25 characters is enough for any 64 bit value.
  if (count > 25) {
    pr_err("Attempt to write unexpectedly long value to sysfs attribute.\n");
    return -EINVAL;
  }
  struct bmp280_hd44780_monitor *monitor = dev_get_drvdata(dev);
  if (mutex_lock_interruptible(&monitor->monitor_mutex)) {
    return -ERESTARTSYS;
  }
  // Copy buffer to local, null-terminated string.
  char str[26];
  memset(str, 0, sizeof(str));
  memcpy(str, buf, count);
  ssize_t ret = 0;
  if (attr == &dev_attr_monitor_display_index) {
    // base=0 means autodetect base
    ret = kstrtos32(str, /*base=*/0, &monitor->display_index);
  } else if (attr == &dev_attr_monitor_refresh_period_ms) {
    // base=0 means autodetect base
    ret = kstrtou32(str, /*base=*/0, &monitor->refresh_period_ms);
    // If currently running, run the next refresh right away, so we don't have
    // to wait for the old refresh period.
    if (ret == 0 && monitor->running) {
      // Ignore return value, this might fail if the the work is already queued
      // up for running, which is normal.
      schedule_delayed_work(&monitor->dwork, /*delay=*/0);
    }
  } else if (attr == &dev_attr_monitor_running) {
    s32 value = monitor->running;
    // base=0 means autodetect base
    ret = kstrtos32(str, /*base=*/0, &value);
    if (ret == 0) {
      if (value == 0) {
	// Stop running
	monitor->running = false;
	cancel_delayed_work(&monitor->dwork);
      } else {
	// Either start running again, or run the next refresh right away.
	monitor->running = true;
	// Ignore return value, this might fail if the the work is already queued
	// up for running, which is normal.
	schedule_delayed_work(&monitor->dwork, /*delay=*/0);
      }
    }
  } else {
    ret = -EINVAL;
  }
  mutex_unlock(&monitor->monitor_mutex);
  if (ret == 0) {
    ret = count;
  }
  return ret;
}

/**
 * Monitor platform driver probe method.
 *
 * Allocates and initializes an instance of our monitor, retrieves references to
 * the BMP280 IIO channels, creates the sysfs attribute files, and start our
 * worker thread (as a system default workqueue entry).
 */
static int bmp280_hd44780_monitor_probe(struct platform_device *pdev) {
  pr_info("Probing bmp280-hd44780-monitor platform driver.\n");
  // Allocate driver context instance
  struct bmp280_hd44780_monitor *monitor =
    devm_kzalloc(&pdev->dev, sizeof(struct bmp280_hd44780_monitor), GFP_KERNEL);
  if (!monitor) {
    return -ENOMEM;
  }
  monitor_init(monitor);
  int ret = 0;
  // Attempt to retrieve temperature channel as a device property
  monitor->temperature_channel =
    devm_iio_channel_get(&pdev->dev, "temperature");
  if (IS_ERR(monitor->temperature_channel)) {
    pr_err("Failed to acquire IIO temperature channel with error %ld. "
	   "Aborting probe.\n", PTR_ERR(monitor->temperature_channel));
    ret = PTR_ERR(monitor->temperature_channel);
    goto out_fail;
  }
  // Attempt to retrieve pressure channel as a device property
  monitor->pressure_channel =
    devm_iio_channel_get(&pdev->dev, "pressure");
  if (IS_ERR(monitor->pressure_channel)) {
    pr_err("Failed to acquire IIO pressure channel with error %ld. "
	   "Aborting probe.\n", PTR_ERR(monitor->pressure_channel));
    ret = PTR_ERR(monitor->pressure_channel);
    goto out_fail;
  }
  // Make our context structure available from this device
  dev_set_drvdata(&pdev->dev, monitor);
  // Setup sysfs files for driver runtime control
  ret = device_create_file(&pdev->dev, &dev_attr_monitor_display_index);
  if (ret) {
    goto out_fail;
  }
  ret = device_create_file(&pdev->dev, &dev_attr_monitor_refresh_period_ms);
  if (ret) {
    goto out_fail;
  }
  ret = device_create_file(&pdev->dev, &dev_attr_monitor_running);
  if (ret) {
    goto out_fail;
  }
  // Start our monitor worker thread
  if (!schedule_delayed_work(&monitor->dwork, /*delay=*/0)) {
    pr_err("Failed to schedule worker thread. Aborting probe.\n");
    ret = -EFAULT;
    goto out_fail;
  }
  pr_info("Successfully probed bmp280-hd44780-monitor platform driver.\n");
  return 0;
 out_fail:
  device_remove_file(&pdev->dev, &dev_attr_monitor_display_index);
  device_remove_file(&pdev->dev, &dev_attr_monitor_refresh_period_ms);
  device_remove_file(&pdev->dev, &dev_attr_monitor_running);
  monitor_teardown(monitor);
  return ret;
}

/**
 * Monitor platform driver remove function.
 *
 * Stops the driver worker thread.
 */
static void bmp280_hd44780_monitor_remove(struct platform_device *pdev) {
  device_remove_file(&pdev->dev, &dev_attr_monitor_display_index);
  device_remove_file(&pdev->dev, &dev_attr_monitor_refresh_period_ms);
  device_remove_file(&pdev->dev, &dev_attr_monitor_running);
  struct bmp280_hd44780_monitor *monitor = dev_get_drvdata(&pdev->dev);
  monitor_teardown(monitor);
  pr_info("Successfully removed bmp280-hd44780-monitor platform driver.\n");
}

/**
 * Device Tree based matching ids (OF = open firmware).
 *
 * Used for auto loading the kernel module, and for driver matching.
 */
static const struct of_device_id bmp280_hd44780_monitor_of_driver_ids[] = {
  {
    .compatible = "leonardo,bmp280-hd44780-monitor",
  },
  { /* sentinel */ },
};

/**
 * HD44780 LCD monitor platform driver for the BMP280 IIO driver.
 */
static struct platform_driver bmp280_hd44780_monitor_driver = {
  .probe = bmp280_hd44780_monitor_probe,
  .remove_new = bmp280_hd44780_monitor_remove,
  .driver = {
    .name = "bmp280-hd44780-monitor",
    .of_match_table = of_match_ptr(bmp280_hd44780_monitor_of_driver_ids),
  },
};

/**
 * This macro gets replaced by a module __init/__exit function pair,
 * that does nothing other than registering/unregistering the platform driver.
*/
module_platform_driver(bmp280_hd44780_monitor_driver);
