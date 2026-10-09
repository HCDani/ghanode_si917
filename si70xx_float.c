/***************************************************************************/ /**
 * @file si70xx_float.c
 * @brief Si70xx reads that keep the fractional part.
 ******************************************************************************/

#include "si70xx_float.h"
#include "sl_si91x_si70xx.h"

/*******************************************************************************
 ***************************   Defines / Macros   ******************************
 ******************************************************************************/
#define SI70XX_CMD_LEN      1 ///< Measurement commands are a single byte
#define SI70XX_EID_CMD_LEN  2 ///< Electronic ID commands are two bytes
#define SI70XX_MEAS_LEN     2 ///< Measurements are returned as 16 bits
#define SI70XX_EID_RESP_LEN 6 ///< Second EID access returns SNB_3..SNB_0 plus two CRCs

/*******************************************************************************
 ***********************  Local function Prototypes ***************************
 ******************************************************************************/
static sl_status_t si70xx_transfer(sl_i2c_instance_t i2c_instance,
                                   uint8_t addr,
                                   const uint8_t *command,
                                   uint32_t command_len,
                                   uint8_t *response,
                                   uint32_t response_len);
static void si70xx_wait_till_i2c_gets_idle(sl_i2c_instance_t i2c_instance);
static void *si70xx_get_i2c_base_address(sl_i2c_instance_t i2c_instance);

/*******************************************************************************
 * Measures temperature in degrees Celsius.
 ******************************************************************************/
sl_status_t si70xx_float_measure_temperature(sl_i2c_instance_t i2c_instance, uint8_t addr, float *temperature_c)
{
  sl_status_t status;
  uint8_t command = (uint8_t)SL_TEMPERATURE_HM;
  uint8_t response[SI70XX_MEAS_LEN];

  if (temperature_c == NULL) {
    return SL_STATUS_NULL_POINTER;
  }

  status = si70xx_transfer(i2c_instance, addr, &command, SI70XX_CMD_LEN, response, SI70XX_MEAS_LEN);
  if (status != SL_STATUS_OK) {
    return status;
  }

  // Si7021 datasheet, section 5.1.1: T = (175.72 * code) / 65536 - 46.85
  uint16_t code  = (uint16_t)((response[0] << 8) | response[1]);
  *temperature_c = ((175.72f * (float)code) / 65536.0f) - 46.85f;
  return SL_STATUS_OK;
}

/*******************************************************************************
 * Measures relative humidity in percent.
 ******************************************************************************/
sl_status_t si70xx_float_measure_humidity(sl_i2c_instance_t i2c_instance, uint8_t addr, float *humidity_pct)
{
  sl_status_t status;
  uint8_t command = (uint8_t)SL_HUMIDITY_HM;
  uint8_t response[SI70XX_MEAS_LEN];

  if (humidity_pct == NULL) {
    return SL_STATUS_NULL_POINTER;
  }

  status = si70xx_transfer(i2c_instance, addr, &command, SI70XX_CMD_LEN, response, SI70XX_MEAS_LEN);
  if (status != SL_STATUS_OK) {
    return status;
  }

  // Si7021 datasheet, section 5.1.1: RH = (125 * code) / 65536 - 6. The formula
  // can yield slightly out-of-range values near the rails, so clamp as the
  // vendor driver does.
  uint16_t code = (uint16_t)((response[0] << 8) | response[1]);
  float value   = ((125.0f * (float)code) / 65536.0f) - 6.0f;
  if (value < 0.0f) {
    value = 0.0f;
  } else if (value > 100.0f) {
    value = 100.0f;
  }
  *humidity_pct = value;
  return SL_STATUS_OK;
}

/*******************************************************************************
 * Measures relative humidity and temperature.
 ******************************************************************************/
sl_status_t si70xx_float_measure_rh_and_temp(sl_i2c_instance_t i2c_instance,
                                             uint8_t addr,
                                             float *humidity_pct,
                                             float *temperature_c)
{
  sl_status_t status;

  if ((humidity_pct == NULL) || (temperature_c == NULL)) {
    return SL_STATUS_NULL_POINTER;
  }

  status = si70xx_float_measure_humidity(i2c_instance, addr, humidity_pct);
  if (status != SL_STATUS_OK) {
    return status;
  }
  return si70xx_float_measure_temperature(i2c_instance, addr, temperature_c);
}

/*******************************************************************************
 * Reads the device ID byte from the second electronic ID access.
 ******************************************************************************/
sl_status_t si70xx_float_read_device_id(sl_i2c_instance_t i2c_instance, uint8_t addr, uint8_t *device_id)
{
  sl_status_t status;
  uint8_t command[SI70XX_EID_CMD_LEN] = { (uint8_t)SL_EID_BYTE21, (uint8_t)SL_EID_BYTE22 };
  uint8_t response[SI70XX_EID_RESP_LEN];

  if (device_id == NULL) {
    return SL_STATUS_NULL_POINTER;
  }

  status = si70xx_transfer(i2c_instance, addr, command, SI70XX_EID_CMD_LEN, response, SI70XX_EID_RESP_LEN);
  if (status != SL_STATUS_OK) {
    return status;
  }

  // Response is SNB_3, SNB_2, CRC, SNB_1, SNB_0, CRC (datasheet Table 12).
  *device_id = response[0];
  return SL_STATUS_OK;
}

/*******************************************************************************
 * Writes a command and reads back the response.
 ******************************************************************************/
static sl_status_t si70xx_transfer(sl_i2c_instance_t i2c_instance,
                                   uint8_t addr,
                                   const uint8_t *command,
                                   uint32_t command_len,
                                   uint8_t *response,
                                   uint32_t response_len)
{
  sl_i2c_status_t i2c_status;
  uint8_t tx_buffer[SI70XX_EID_CMD_LEN];

  if (i2c_instance >= SL_I2C_LAST) {
    return SL_STATUS_INVALID_PARAMETER;
  }

  // sl_i2c_driver_send_data_blocking() takes a non-const buffer.
  for (uint32_t i = 0; i < command_len; i++) {
    tx_buffer[i] = command[i];
  }

  // Si7021 EID and hold-master conversions are write-then-read with a repeated
  // start. Do not wait for ACTIVITY=0 between the phases: that wait completes
  // only after a STOP, which aborts the command. The blocking receive then
  // times out with SL_I2C_TIMEOUT (0x10) because no payload is produced.
  i2c_status = sl_i2c_driver_enable_repeated_start(i2c_instance, true);
  if (i2c_status != SL_I2C_SUCCESS) {
    return SL_STATUS_IO;
  }

  i2c_status = sl_i2c_driver_send_data_blocking(i2c_instance, addr, tx_buffer, command_len);
  if (i2c_status != SL_I2C_SUCCESS) {
    (void)sl_i2c_driver_enable_repeated_start(i2c_instance, false);
    return SL_STATUS_IO;
  }

  i2c_status = sl_i2c_driver_enable_repeated_start(i2c_instance, false);
  if (i2c_status != SL_I2C_SUCCESS) {
    return SL_STATUS_IO;
  }

  i2c_status = sl_i2c_driver_receive_data_blocking(i2c_instance, addr, response, response_len);
  if (i2c_status != SL_I2C_SUCCESS) {
    return SL_STATUS_IO;
  }
  si70xx_wait_till_i2c_gets_idle(i2c_instance);

  return SL_STATUS_OK;
}

/*******************************************************************************
 * Spins until the I2C block reports the bus is no longer active. The blocking
 * transfer APIs return once the last byte is clocked out, which is before the
 * stop condition has completed.
 ******************************************************************************/
static void si70xx_wait_till_i2c_gets_idle(sl_i2c_instance_t i2c_instance)
{
  I2C0_Type *i2c = (I2C0_Type *)si70xx_get_i2c_base_address(i2c_instance);

  if (i2c == NULL) {
    return;
  }
  while (i2c->IC_STATUS_b.ACTIVITY) {
  }
}

/*******************************************************************************
 * Maps an I2C instance to its peripheral base address.
 ******************************************************************************/
static void *si70xx_get_i2c_base_address(sl_i2c_instance_t i2c_instance)
{
  I2C0_Type *i2c = NULL;

  if (i2c_instance == SL_I2C0) {
    i2c = ((I2C0_Type *)I2C0_BASE);
  } else if (i2c_instance == SL_I2C1) {
    i2c = ((I2C0_Type *)I2C1_BASE);
  } else if (i2c_instance == SL_ULP_I2C) {
    i2c = ((I2C0_Type *)I2C2_BASE);
  }
  return i2c;
}
