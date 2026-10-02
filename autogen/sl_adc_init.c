/***************************************************************************//**
 * @file sl_adc_init.c.jinja
 * @brief ADC Driver Instance Initialization
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/

#include "sl_si91x_adc.h"

#include "sl_si91x_adc_init_channel_1_config.h"


#include "sl_adc_instances.h"


sl_adc_channel_config_t sl_adc_channel_config = {

  .input_type[SL_ADC_CHANNEL_1]     = SL_ADC_CHANNEL_1_INPUT_TYPE,
  .pos_inp_sel[SL_ADC_CHANNEL_1]    = SL_ADC_CHANNEL_1_POS_INPUT_CHNL_SEL,
  .neg_inp_sel[SL_ADC_CHANNEL_1]    = SL_ADC_CHANNEL_1_NEG_INPUT_CHNL_SEL,
  .num_of_samples[SL_ADC_CHANNEL_1] = SL_ADC_CHANNEL_1_SAMPLE_LENGTH,
  .sampling_rate[SL_ADC_CHANNEL_1]  = SL_ADC_CHANNEL_1_SAMPLING_RATE,

};
