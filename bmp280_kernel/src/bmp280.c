#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/printk.h>

#include "bmp280.h"

/**
 * Performs a device id sanity check, then initializes the BMP280 sensor.
 * We are using the following configuration:
 *     * maximum temperature and pressure oversampling (x16): this gives us
 * 20 bits of resolution.
 *     * Normal power mode: the sensor will continuously collecting samples.
 *     * 1000ms standby mode: samples are collected once per second.
 *     * No filtering: disable data smoothing over time.
 *     * No 3-wire SPI: we only use I2C
 */
static int initialize_bmp280(struct bmp280_ctx *bmp280) {
  // Try to read the sensor ID, and verify if it matches the expected BMP280 ID.
  u8 sensor_id = i2c_smbus_read_byte_data(bmp280->client, BMP280_ID_REG);
  if (sensor_id != BMP280_ID) {
    pr_err("Unexpected sensor id 0x%02x. Expecting 0x%02x\n",
	   sensor_id, BMP280_ID);
    return -ENODEV;
  }
  // Maximum temperature oversampling (x16)
  u8 osrs_t = 0x5;
  // Maximum pressure oversampling (x16)
  u8 osrs_p = 0x5;
  // Normal power mode
  u8 mode = 0x3;
  // Standby time. In normal power mode, take a measurement every 1000ms (1s)
  u8 t_sb = 0x5;
  // No filtering
  u8 filter = 0x0;
  // No 3-wire SPI interface. We only use I2C
  u8 spi3w_en = 0x0;
  // These options are combined into the ctrl_meas and config registers
  u8 ctrl_meas = (osrs_t << 5) | (osrs_p << 2) | mode;
  u8 config = (t_sb << 5) | (filter << 2) | spi3w_en;
  i2c_smbus_write_byte_data(bmp280->client, BMP280_CONFIG_REG_ADDRESS, config);
  i2c_smbus_write_byte_data(bmp280->client, BMP280_CTRL_MEAS_REG_ADDRESS,
			    ctrl_meas);
  return 0;
}

/**
 * Reads the BMP280 constant calibration values, and stores them in the context
 * structure's dig_T and dig_P fields.
 * These are 16 bit values, stored from 0x88 to 0xa1 on the sensor register
 * bank.
 */
static int read_bmp280_calibration_values(struct bmp280_ctx *bmp280) {
  // Read all temperature calibration values, then all pressure calibration
  // values, to minimize the number of I2C reads during setup.
  // Note: `n_read` is specified in bytes.
  u16 temp_calib_buffer[3];
  s32 read = i2c_smbus_read_i2c_block_data(
      bmp280->client, BMP280_TEMP_CALIBRATION_BASE_REG_ADDRESS, 
      /*n_read=*/3 * 2, (u8 *)temp_calib_buffer);
  if (read != 3 * 2) {
    pr_err("Expected 6 temperature calibration bytes. Read %d instead\n", read);
    return -EIO;
  }
  u16 press_calib_buffer[9];
  read = i2c_smbus_read_i2c_block_data(
      bmp280->client, BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS,
      /*n_read=*/9 * 2, (u8 *)press_calib_buffer);
  if (read != 9 * 2) {
    pr_err("Expected 18 pressure calibration bytes. Read %d instead\n", read);
    return -EIO;
  }
  // ignoring index zero to match the indexes starting from 1 in the
  // datasheet algorithm.
  bmp280->dig_T[0] = 0;
  bmp280->dig_P[0] = 0;
  // dig_T1 and dig_P1 should be treated as unsigned 16 bits, whereas all other
  // calibration values should be treated as signed 16 bits.
  for (int i = 1; i <= 3; i++) {
    bmp280->dig_T[i] = temp_calib_buffer[i-1];
    if(i != 1 && bmp280->dig_T[i] > 32767) {
      bmp280->dig_T[i] -= 65536;
    }
  }
  for (int i = 1; i <= 9; i++) {
    bmp280->dig_P[i] = press_calib_buffer[i-1];
    if(i != 1 && bmp280->dig_P[i] > 32767) {
      bmp280->dig_P[i] -= 65536;
    }
  }
  return 0;
}

/**
 * Calls sensor initialization functions, then reads the constant calibration
 * values from the sensor and sets the BMP280 context structure.
 */
int setup_bmp280(struct i2c_client *client, struct bmp280_ctx *bmp280) {
  // Make the I2C client available from the context structure
  bmp280->client = client;
  // Initialize sensor
  int status = initialize_bmp280(bmp280);
  if (status) {
    return status;
  }
  status = read_bmp280_calibration_values(bmp280);
  if (status) {
    return status;
  }
  return 0;
}

/**
 * Reads the raw temperature value from the sensor.
 * It takes up the 20 MS bits of three consecutive 8 bit registers.
 * We read the three registers at once so we don't run the risk of the sensor
 * updating them while we are reading.
 */
int read_bmp280_raw_temperature(struct bmp280_ctx *bmp280, s32 *raw_temp) {
  u8 values[3] = {0, 0, 0};
  s32 read = i2c_smbus_read_i2c_block_data(
      bmp280->client, BMP280_TEMP_RAW_REG_ADDRESS, /*length=*/3, values);
  if (read != 3) {
    pr_err("Expected to read 3 raw temperature bytes. Read %d instead\n", read);
    return -EIO;
  }
  s32 val1 = values[0];
  s32 val2 = values[1];
  s32 val3 = values[2];
  // The LS 4 bits of val3 are irrelevant. We do not right shift on this method,
  // we just return the raw value, as read from the sensor.
  *raw_temp = ((val1 << 16) | (val2 << 8) | val3);
  return 0;
}

/**
 * Reads the raw pressure value from the sensor.
 * It takes up the 20 MS bits of three consecutive 8 bit registers.
 * We read the three registers at once so we don't run the risk of the sensor
 * updating them while we are reading.
 */
int read_bmp280_raw_pressure(struct bmp280_ctx *bmp280, s32 *raw_press) {
  u8 values[3] = {0, 0, 0};
  s32 read = i2c_smbus_read_i2c_block_data(
      bmp280->client, BMP280_PRESS_RAW_REG_ADDRESS, /*length=*/3, values);
  if (read != 3) {
    pr_err("Expected to read 3 raw pressure bytes. Read %d instead\n", read);
    return -EIO;
  }
  s32 val1 = values[0];
  s32 val2 = values[1];
  s32 val3 = values[2];
  // The LS 4 bits of val3 are irrelevant. We do not right shift on this method,
  // we just return the raw value, as read from the sensor.
  *raw_press = ((val1 << 16) | (val2 << 8) | val3);
  return 0;
}

/**
 * `t_fine` is an intermediate temperature value, required by both the final
 * processed temperature, as well as for pressure computation. See the
 * datasheet for details.
 * https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf
 * (Section 3.11.3 - Compensation formula)
 */
static s32 compute_bmp280_t_fine(s32 raw_temp, const s32 dig_T[]) {
  // This rather cryptic set of operations is described in the datasheet
  s32 var1 = (((raw_temp >> 3) - (dig_T[1] << 1)) * dig_T[2]) >> 11;
  s32 var2 = (((((raw_temp >> 4) - dig_T[1]) * ((raw_temp >> 4) - dig_T[1])) >> 12)
	      * dig_T[3]) >> 14;
  return var1 + var2;
}

/**
 * Computes the final temperature, in units of 1/100 degrees Celcius.
 * We do this using the calibration values and the conversion algorithm
 * described in the datasheet.
 * https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf
 * (Section 3.11.3 - Compensation formula)
 */
int read_bmp280_processed_temperature(struct bmp280_ctx *bmp280, s32 *temp) {
  s32 raw_temp;
  int status = read_bmp280_raw_temperature(bmp280, &raw_temp);
  if (status) {
    return status;
  }
  // LS 4 bits of raw temperature are ignored.
  raw_temp >>= 4;
  *temp = (compute_bmp280_t_fine(raw_temp, bmp280->dig_T) * 5 + 128) >> 8;
  return 0;
}

/**
 * Computes the final pressure, as an unsigned 32 bit integer,
 * in units of 1 / 256 Pascal.
 * We do this using the calibration values and the conversion algorithm
 * described in the datasheet.
 * https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf
 * (Section 3.11.3 - Compensation formula)
 */
int read_bmp280_processed_pressure(struct bmp280_ctx *bmp280, u32 *press) {
  // We need both the raw temperature, and the raw pressure values to compute
  // the final pressure. Pressure registers come before. We read all of them
  // at once to avoid the risk of the sensor chaning either of them in between
  // reads.
  u8 values[6];
  int read = i2c_smbus_read_i2c_block_data(
      bmp280->client, BMP280_PRESS_RAW_REG_ADDRESS, /*length=*/6, values);
  if (read != 6) {
    pr_err("Expected to read 6 temperature/pressure bytes. Read %d instead\n",
	   read);
    return -EIO;
  }
  s32 p1 = values[0];
  s32 p2 = values[1];
  s32 p3 = values[2];
  s32 raw_press = (p1 << 16) | (p2 << 8) | p3;
  s32 t1 = values[3];
  s32 t2 = values[4];
  s32 t3 = values[5];
  s32 raw_temp = (t1 << 16) | (t2 << 8) | t3;
  // LS 4 bits of raw temperature and pressure are ignored.
  raw_press >>= 4;
  raw_temp >>= 4;
  s64 t_fine = compute_bmp280_t_fine(raw_temp, bmp280->dig_T);
  s64 var1 = t_fine - 128000;
  s64 var2 = var1 * var1 * bmp280->dig_P[6];
  var2 = var2 + ((var1 * bmp280->dig_P[5]) << 17);
  var2 = var2 + (bmp280->dig_P[4] << 35);
  var1 = (((var1 * var1 * bmp280->dig_P[3]) >> 8) +
	  ((var1 * bmp280->dig_P[2]) << 12));
  var1 = ((((s64)1) << 47) + var1) * bmp280->dig_P[1] >> 33;
  if (var1 == 0) {
    *press = 0;
    return 0;
  }
  s64 p = 1048576 - raw_press;
  p = (((p << 31) - var2) * 3125) / var1;
  var1 = (bmp280->dig_P[9] * (p >> 13) * (p >> 13)) >> 25;
  var2 = (bmp280->dig_P[8] * p) >> 19;
  p = ((p + var1 + var2) >> 8) + (bmp280->dig_P[7] << 4);
  *press = (u32)p;
  return 0;
}
