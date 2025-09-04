#ifndef BMP280_H_
#define BMP280_H_

#include <linux/i2c.h>
#include <linux/types.h>

/**
 * Used as a sanity check during sensor initialization.
 * If we are really talking with a real BMP280 sensor, then reading from the
 * BMP280_ID_REG register will return us BMP280_ID.
 */
#define BMP280_ID 0x58
#define BMP280_ID_REG 0xD0

/**
 * Register addresses for sampling control and configuration.
 */
#define BMP280_CTRL_MEAS_REG_ADDRESS 0xf4
#define BMP280_CONFIG_REG_ADDRESS 0xf5

/**
 * Register addresses for temperature reading.
 */
#define BMP280_TEMP_CALIBRATION_BASE_REG_ADDRESS 0x88
#define BMP280_TEMP_RAW_REG_ADDRESS 0xfa

/**
 * Register addresses for pressure reading.
 */
#define BMP280_PRESS_CALIBRATION_BASE_REG_ADDRESS 0x8e
#define BMP280_PRESS_RAW_REG_ADDRESS 0xf7

/**
 * BMP280 context structure.
 * dig_T and dig_P are the sensor's calibration values, which are constant for
 * any given sensor, so we only read them once and keep store them here.
 */
struct bmp280_ctx {
  struct i2c_client *client;
  s32 dig_T[4];
  s64 dig_P[10];
};

/**
 * Sets up an IIO device and registers it with the IIO subsystem.
 */
int register_bmp280_iio_device(struct i2c_client *client);

// BMP280 I2C communication methods

/**
 * Calls sensor initialization functions, then reads the constant calibration
 * values from the sensor and sets the BMP280 context structure.
 */
int setup_bmp280(struct i2c_client *client, struct bmp280_ctx *bmp280);

/**
 * Reads the raw temperature value from the sensor.
 * It takes up the 20 MS bits of three consecutive 8 bit registers.
 */
int read_bmp280_raw_temperature(struct bmp280_ctx *bmp280, s32 *raw_temp);

/**
 * Reads the raw pressure value from the sensor.
 * It takes up the 20 MS bits of three consecutive 8 bit registers.
 */
int read_bmp280_raw_pressure(struct bmp280_ctx *bmp280, s32 *raw_press);

/**
 * Computes the final temperature, in units of 1/100 degrees Celcius.
 */
int read_bmp280_processed_temperature(struct bmp280_ctx *bmp280, s32 *temp);

/**
 * Computes the final pressure, as an unsigned 32 bit integer,
 * in units of 1/256 Pascal.
 */
int read_bmp280_processed_pressure(struct bmp280_ctx *bmp280, u32 *press);

#endif  // BMP280_H_
