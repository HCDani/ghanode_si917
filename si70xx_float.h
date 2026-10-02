/***************************************************************************/ /**
 * @file si70xx_float.h
 * @brief Si70xx reads that keep the fractional part.
 *******************************************************************************
 * The WiSeConnect driver (sl_si91x_si70xx) computes the datasheet conversion in
 * float and then casts the result to an integer, so it can only ever report
 * whole degrees Celsius and whole percent RH. The Gecko Platform driver for
 * EFR32 parts returns milli-units, but it is built on I2CSPM/emlib and does not
 * exist for the si91x. This is a minimal re-implementation of the same
 * transfers that returns the converted values as floats.
 ******************************************************************************/

#ifndef SI70XX_FLOAT_H
#define SI70XX_FLOAT_H

#include "sl_si91x_i2c.h"
#include "sl_status.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// SNB_3 byte of the electronic ID identifies the part (Si7021 datasheet, Table 12).
#define SI70XX_DEVICE_ID_SI7013 0x0D
#define SI70XX_DEVICE_ID_SI7020 0x14
#define SI70XX_DEVICE_ID_SI7021 0x15

/***************************************************************************/ /**
 * @brief Measures temperature in degrees Celsius (hold master mode).
 *
 * @pre sl_i2c_driver_init() must have been called for the instance.
 *
 * @param[in]  i2c_instance   I2C instance the sensor is wired to.
 * @param[in]  addr           I2C address of the sensor.
 * @param[out] temperature_c  Temperature in degrees Celsius.
 *
 * @return SL_STATUS_OK on success, SL_STATUS_NULL_POINTER on a NULL argument,
 *         SL_STATUS_INVALID_PARAMETER on a bad instance, SL_STATUS_IO on a
 *         failed transfer.
 ******************************************************************************/
sl_status_t si70xx_float_measure_temperature(sl_i2c_instance_t i2c_instance, uint8_t addr, float *temperature_c);

/***************************************************************************/ /**
 * @brief Measures relative humidity in percent (hold master mode).
 *
 * @pre sl_i2c_driver_init() must have been called for the instance.
 *
 * @param[in]  i2c_instance  I2C instance the sensor is wired to.
 * @param[in]  addr          I2C address of the sensor.
 * @param[out] humidity_pct  Relative humidity in percent, clamped to 0..100.
 *
 * @return SL_STATUS_OK on success, see si70xx_float_measure_temperature() for
 *         the error codes.
 ******************************************************************************/
sl_status_t si70xx_float_measure_humidity(sl_i2c_instance_t i2c_instance, uint8_t addr, float *humidity_pct);

/***************************************************************************/ /**
 * @brief Measures relative humidity and temperature in one call.
 *
 * @details Issues an RH conversion followed by a separate temperature
 * conversion, matching the behaviour of sl_si91x_si70xx_measure_rh_and_temp().
 *
 * @pre sl_i2c_driver_init() must have been called for the instance.
 *
 * @param[in]  i2c_instance   I2C instance the sensor is wired to.
 * @param[in]  addr           I2C address of the sensor.
 * @param[out] humidity_pct   Relative humidity in percent, clamped to 0..100.
 * @param[out] temperature_c  Temperature in degrees Celsius.
 *
 * @return SL_STATUS_OK on success, see si70xx_float_measure_temperature() for
 *         the error codes.
 ******************************************************************************/
sl_status_t si70xx_float_measure_rh_and_temp(sl_i2c_instance_t i2c_instance,
                                             uint8_t addr,
                                             float *humidity_pct,
                                             float *temperature_c);

/***************************************************************************/ /**
 * @brief Reads the device ID byte from the second electronic ID access.
 *
 * @details sl_si91x_si70xx_is_present() performs this transfer but discards the
 * payload, so it cannot tell you which part answered.
 *
 * @pre sl_i2c_driver_init() must have been called for the instance.
 *
 * @param[in]  i2c_instance  I2C instance the sensor is wired to.
 * @param[in]  addr          I2C address of the sensor.
 * @param[out] device_id     SNB_3 byte, compare against SI70XX_DEVICE_ID_*.
 *
 * @return SL_STATUS_OK on success, see si70xx_float_measure_temperature() for
 *         the error codes.
 ******************************************************************************/
sl_status_t si70xx_float_read_device_id(sl_i2c_instance_t i2c_instance, uint8_t addr, uint8_t *device_id);

#ifdef __cplusplus
}
#endif

#endif // SI70XX_FLOAT_H
