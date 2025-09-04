#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/types.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "bmp280.h"

/**
 * IIO channel macro for calibration values.
 * For triggered buffer reads, `scan_index` sets the position of this channel
 * data within the sample. `scan_type` tells that the channel data takes up 16
 * bits without any padding, and follows the CPU's endianness.
 */
#define BMP280_CALIBR_CHANNNEL(_type, _index, _scan_index, _sign, _address) { \
    .type = (_type),							\
    .indexed = 1,							\
    .channel = (_index),						\
    .address = (_address),						\
    .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
    .scan_index = (_scan_index),					\
    .scan_type = {							\
      .sign = (_sign),							\
      .realbits = 16,							\
      .storagebits = 16,						\
      .shift = 0,							\
      .endianness = IIO_CPU,						\
    },									\
    .output = 0,							\
}

/**
 * IIO channels.
 * We make the following channels available:
 *     * Three temperature calibration values.
 *     * One raw temperature value.
 *     * One final, processed temperature value.
 *     * Nine pressure calibration values.
 *     * One raw pressure value.
 *     * One final, processed pressure value.
 * Within `/sys/bus/iio/devices/iio:deviceX/`, these will be:
 * `in_temp{0-3}_raw`, `in_temp_input`, `in_pressure{0-9}_raw`, and
 * `in_pressure_input`, respectively.
 */
static const struct iio_chan_spec bmp280_iio_channels[] = {
  // Temperature calibration values, refered to as dig_T1 to dig_T3 on the
  // datasheet. Corresponding sysfs files: `in_temp{0-2}_raw`.
  // Note: each calibration value is 16 bits, thus the address deltas.
  BMP280_CALIBR_CHANNNEL(IIO_TEMP, 0, 0, 'u',
			 BMP280_TEMP_CALIBRATION_BASE_REG_ADDRESS),
  BMP280_CALIBR_CHANNNEL(IIO_TEMP, 1, 1, 's',
			 BMP280_TEMP_CALIBRATION_BASE_REG_ADDRESS + 2),
  BMP280_CALIBR_CHANNNEL(IIO_TEMP, 2, 2, 's',
			 BMP280_TEMP_CALIBRATION_BASE_REG_ADDRESS + 4),
  // Raw temperature value, as directly read from the sensor.
  // Corresponding sysfs file: `in_temp3_raw`
  {
    .type = IIO_TEMP,
    .indexed = 1,
    .channel = 3,
    .address = BMP280_TEMP_RAW_REG_ADDRESS,
    .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
    .scan_index = 3,
    // Channel data is signed (2 complement), takes up 20 bits within a 32 bits
    // field, with the 4 LS bits being padding bits, and follows the host
    // CPU's endianness.
    .scan_type = {
      .sign = 's',
      .realbits = 20,
      .storagebits = 32,
      .shift = 4,
      .endianness = IIO_CPU,
    },
    .output = 0,
  },
  // Final "processed" temperature value.
  // Corresponding sysfs file: `in_temp_input`
  {
    .type = IIO_TEMP,
    .indexed = 0,
    .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
    .scan_index = 4,
    // Channel data is signed (2 complement), takes up 32 bits,
    // and follows the host CPU's endianness.
    .scan_type = {
      .sign = 's',
      .realbits = 32,
      .storagebits = 32,
      .shift = 0,
      .endianness = IIO_CPU,
    },
    .output = 0,
  },
  // Pressure calibration values, refered to as dig_P1 to dig_P9 on the
  // datasheet. Corresponding sysfs files: `in_pressure{0-8}_raw`.
  // Note: each calibration value is 16 bits, thus the address deltas.
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 0, 5, 'u',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 1, 6, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 2),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 2, 7, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 4),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 3, 8, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 6),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 4, 9, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 8),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 5, 10, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 10),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 6, 11, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 12),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 7, 12, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 14),
  BMP280_CALIBR_CHANNNEL(IIO_PRESSURE, 8, 13, 's',
			 BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS + 16),
  // Raw pressure value, as directly read from the sensor.
  // Corresponding sysfs file: `in_pressure9_raw`
  {
    .type = IIO_PRESSURE,
    .indexed = 1,
    .channel = 9,
    .address = BMP280_PRESS_RAW_REG_ADDRESS,
    .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
    .scan_index = 14,
    // Channel data is signed (2 complement), takes up 20 bits within a 32 bits
    // field, with the 4 LS bits being padding bits, and follows the host
    // CPU's endianness.
    .scan_type = {
      .sign = 's',
      .realbits = 20,
      .storagebits = 32,
      .shift = 4,
      .endianness = IIO_CPU,
    },
    .output = 0,
  },
  // Final "processed" pressure value.
  // Corresponding sysfs file: `in_pressure_input`
  {
    .type = IIO_PRESSURE,
    .indexed = 0,
    .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
    .scan_index = 15,
    // Channel data is unsigned, takes up 32 bits,
    // and follows the host CPU's endianness.
    .scan_type = {
      .sign = 'u',
      .realbits = 32,
      .storagebits = 32,
      .shift = 0,
      .endianness = IIO_CPU,
    },
    .output = 0,
  },
};

static int bmp280_iio_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask);
static irqreturn_t bmp280_iio_trigger_handler(int irq, void *p);

/**
 * IIO hooks. We only need to read from the device.
 */
static const struct iio_info bmp280_iio_info = {
  .read_raw = bmp280_iio_read_raw,
};

/**
 * Sets up an IIO device and registers it with the IIO subsystem.
 */
int register_bmp280_iio_device(struct i2c_client *client) {
  // Allocate IIO device structure.
  // devm_* methods do not require corresponding free/unregister calls.
  // When client->dev is removed, the reverse operation happens automatically.
  struct iio_dev *indio_dev =
    devm_iio_device_alloc(&client->dev,
			  /*sizeof_priv*/sizeof(struct bmp280_ctx));
  if (!indio_dev) {
    return -ENOMEM;
  }
  indio_dev->dev.parent = &client->dev;
  indio_dev->name = client->name;
  indio_dev->info = &bmp280_iio_info;
  indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;
  indio_dev->channels = bmp280_iio_channels;
  indio_dev->num_channels = ARRAY_SIZE(bmp280_iio_channels);
  struct bmp280_ctx *bmp280 = iio_priv(indio_dev);
  int status = setup_bmp280(client, bmp280);
  if (status) {
    pr_err("Failed to setup BMP280 device.");
    return status;
  }
  // iio_pollfunc_store_time is the top-half IRQ handler, which means it runs in
  // interrupt context. It is defined by the IIO core, and its only work is to
  // record the current timestamp.
  // bmp280_iio_trigger_handler is our bottom half, which does the real trigger
  // handling. It runs in a kernel thread, which means we can perform operations
  // that might block, like talking with the sensor over I2C.
  status = devm_iio_triggered_buffer_setup(&client->dev, indio_dev,
					   iio_pollfunc_store_time,
					   bmp280_iio_trigger_handler, NULL);
  if (status) {
    pr_err("Failed to setup IIO triggered buffer support.");
    return status;
  }
  // Register device with the IIO subsystem
  status = devm_iio_device_register(&client->dev, indio_dev);
  if (status) {
    pr_err("Failed to register with IIO subsystem.");
    return status;
  }
  return 0;
}

/**
 * IIO driver's read method.
 * This method identifies which of the IIO channels is being requested,
 * and assembles the result from sensor specific methos.
 */
static int bmp280_iio_read_from_channel(struct iio_dev *indio_dev,
					struct iio_chan_spec const *chan,
					int *val, int *val2) {
  struct bmp280_ctx *bmp280 = iio_priv(indio_dev);
  if (chan->type == IIO_TEMP) {
    if (iio_channel_has_info(chan, IIO_CHAN_INFO_RAW) && chan->indexed &&
	0 <= chan->channel && chan->channel < 3) {
      // One of the constant temperature calibration values.
      *val = bmp280->dig_T[chan->channel + 1];
      return IIO_VAL_INT;      
    } else if (iio_channel_has_info(chan, IIO_CHAN_INFO_RAW) && chan->indexed &&
	       chan->channel == 3) {
      // Raw temperature value
      s32 raw_temp;
      int status = read_bmp280_raw_temperature(bmp280, &raw_temp);
      if (status) {
	return status;
      }
      *val = raw_temp;
      return IIO_VAL_INT;
    } else if (iio_channel_has_info(chan, IIO_CHAN_INFO_PROCESSED)) {
      // Processed temperature value. *val contains the temperature in
      // 100ths of Celcius. So we set *val2 and return IIO_VAL_FRACTIONAL such
      // that the IIO core produces a fractional value to userspace.
      s32 temp;
      int status = read_bmp280_processed_temperature(bmp280, &temp);
      if (status) {
	return status;
      }
      *val = temp;
      *val2 = 100;
      return IIO_VAL_FRACTIONAL;
    } else {
      pr_err("Unexpected temperature channel\n");
      return -EINVAL;
    }
  } else if(chan->type == IIO_PRESSURE) {
    if (iio_channel_has_info(chan, IIO_CHAN_INFO_RAW) && chan->indexed &&
	0 <= chan->channel && chan->channel < 9) {
      // One of the constant pressure calibration values.
      *val = bmp280->dig_P[chan->channel + 1];
      return IIO_VAL_INT;
    } else if (iio_channel_has_info(chan, IIO_CHAN_INFO_RAW) && chan->indexed &&
	       chan->channel == 9) {
      // Raw pressure value
      s32 raw_press;
      int status = read_bmp280_raw_pressure(bmp280, &raw_press);
      if (status) {
	return status;
      }
      *val = raw_press;
      return IIO_VAL_INT;
    } else if (iio_channel_has_info(chan, IIO_CHAN_INFO_PROCESSED)) {
      // Processed pressure value. *val contains the pressure in
      // 1/256 of Pascal. So we set *val2 and return IIO_VAL_FRACTIONAL such
      // that the IIO core produces a fractional value to userspace.
      u32 press;
      int status = read_bmp280_processed_pressure(bmp280, &press);
      if (status) {
	return status;
      }
      *val = press;
      *val2 = 256;
      return IIO_VAL_FRACTIONAL;
    } else {
      pr_err("Unexpected pressure channel\n");
      return -EINVAL;
    }
  } else {
    pr_err("Unexpected channel type: %d\n", chan->type);
    return -EINVAL;
  }
}

/**
 * IIO driver's read method.
 * This method is callbed when directly reading from the sysfs channel files.
 */
static int bmp280_iio_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long mask) {
  return bmp280_iio_read_from_channel(indio_dev, chan, val, val2);
}

/**
 * IIO driver's triggered buffered handler.
 * This method is called (in a separate kernel thread), for each fired trigger,
 * when using triggered buffer mode.
 * It computes the result for each of a subset of enabled channels,
 * and assembles them together into a buffer, according to each channel's
 * scan_type information.
 */
static irqreturn_t bmp280_iio_trigger_handler(int irq, void *p) {
  struct iio_poll_func *pf = (struct iio_poll_func *)p;
  struct iio_dev *indio_dev = pf->indio_dev;
  void *data = kzalloc(indio_dev->scan_bytes, GFP_KERNEL);
  if (!data) {
    pr_err("Failed allocating memory during trigger handling.\n");
    goto trigger_notify;
  }
  void *data_ptr = data;
  int i;
  for_each_set_bit(i, indio_dev->active_scan_mask, indio_dev->num_channels) {
    const struct iio_chan_spec *chan = &indio_dev->channels[i];
    int val, val2; // val2 is not used here.
    int status = bmp280_iio_read_from_channel(indio_dev, chan, &val, &val2);
    if (status < 0) {
      pr_err("Failed to read from channel #%d.\n", i);
      goto free_data;
    }
    // Store data, handling the possible data types.
    if (chan->scan_type.storagebits == 16) {
      if (chan->scan_type.sign == 's') {
	*((s16 *)data_ptr) = (s16)val;
      } else {
	*((u16 *)data_ptr) = (u16)val;
      }
      data_ptr += 2; // Advance 16 bits
    } else if (chan->scan_type.storagebits == 32) {
      if (chan->scan_type.sign == 's') {
	*((s32 *)data_ptr) = (s32)val;
      } else {
	*((u32 *)data_ptr) = (u32)val;
      }
      data_ptr += 4; // Advance 32 bits
    } else {
      pr_err("Unexpected channel storage bits %d.\n",
	     chan->scan_type.storagebits);
      goto free_data;
    }
  }
  int status = iio_push_to_buffers(indio_dev, data);
  if (status) {
    pr_err("Failed to push data to IIO buffers.\n");
    goto free_data;
  }
 free_data:
  kfree(data);
 trigger_notify:
  iio_trigger_notify_done(indio_dev->trig);
  return IRQ_HANDLED;
}
