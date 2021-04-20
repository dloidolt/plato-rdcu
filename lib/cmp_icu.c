/**
 * @file   icu_cmp.c
 * @author Dominik Loidolt (dominik.loidolt@univie.ac.at),
 * @date   2020
 *
 * @copyright GPLv2
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * @brief software software compression library
 * @see Data Compression User Manual PLATO-UVIE-PL-UM-0001
 */


#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

#include "../include/cmp_support.h"
#include "../include/n_dpu_pkt.h"
#include "../include/cmp_icu.h"
#include "../include/byteorder.h"
#include "../include/cmp_debug.h"


/**
 * @brief check if the compressor configuration is valid for a SW compression,
 *	see the user manual for more information (PLATO-UVIE-PL-UM-0001).
 *
 * @param cfg	configuration contains all parameters required for compression
 * @param info	compressor information contains information of an executed
 *		compression (can be NULL)
 *
 * @returns 0 when configuration is valid, invalid configuration otherwise
 */

int icu_cmp_cfg_valid(const struct cmp_cfg *cfg, struct cmp_info *info)
{
	int cfg_invalid = 0;

	if (!cfg) {
		debug_print("Error: compression configuration structure is NULL.\n");
		return -1;
	}

	if (!info)
		debug_print("Warning: compressor information structure is NULL.\n");

	if (info)
		info->cmp_err = 0;  /* reset errors */

	if (cfg->input_buf == NULL) {
		debug_print("Error: The input_buf buffer for the data to be compressed is NULL.\n");
		cfg_invalid++;
	}

	if (cfg->samples == 0) {
		debug_print("Warning: The samples parameter is 0. No data are compressed. This behavior may not be intended.\n");
	}

	/* icu_output_buf can be NULL if rdcu compression is used */
	if (cfg->icu_output_buf == NULL) {
		debug_print("Error: The icu_output_buf buffer for the compressed data is NULL.\n");
		cfg_invalid++;
	}

	if (cfg->buffer_length == 0 && cfg->samples != 0) {
		debug_print("Error: The buffer_length is set to 0. There is no space to store the compressed data.\n");
		cfg_invalid++;
	}

	if (cfg->icu_output_buf == cfg->input_buf) {
		debug_print("Error: The icu_output_buf buffer is the same as the input_buf buffer.\n");
		cfg_invalid++;
	}

	if (model_mode_is_used(cfg->cmp_mode)) {
		if (cfg->model_buf == NULL) {
			debug_print("Error: The model_buf buffer for the model data is NULL.\n");
			cfg_invalid++;
		}

		if (cfg->model_buf == cfg->input_buf) {
			debug_print("Error: The model_buf buffer is the same as the input_buf buffer.\n");
			cfg_invalid++;
		}

		if (cfg->model_buf == cfg->icu_output_buf) {
			debug_print("Error: The model_buf buffer is the same as the icu_output_buf buffer.\n");
			cfg_invalid++;
		}

		if (cfg->icu_new_model_buf == cfg->input_buf) {
			debug_print("Error: The icu_new_model_buf buffer is the same as the input_buf buffer.\n");
			cfg_invalid++;
		}

		if (cfg->icu_new_model_buf == cfg->icu_output_buf) {
			debug_print("Error: The icu_output_buf buffer is the same as the icu_output_buf buffer.\n");
			cfg_invalid++;
		}
	}

	if (raw_mode_is_used(cfg->cmp_mode)) {
		if (cfg->samples > cfg->buffer_length) {
			debug_print("Error: The buffer_length is to small to hold the data form the input_buf.\n");
			cfg_invalid++;
		}
	} else {
		if (cfg->samples*size_of_a_sample(cfg->cmp_mode) <
		    cfg->buffer_length*sizeof(uint16_t)/3) /* TODO: have samples and buffer_lengt the same unit */
			debug_print("Warning: The size of the icu_output_buf is 3 times smaller than the input_buf. This is probably unintentional.\n");
	}


	if (!(diff_mode_is_used(cfg->cmp_mode)
	     || model_mode_is_used(cfg->cmp_mode)
	     || raw_mode_is_used(cfg->cmp_mode))) {
		debug_print("Error: selected cmp_mode: %lu is not supported\n.",
			    cfg->cmp_mode);
		if (info)
			info->cmp_err |= 1UL << CMP_MODE_ERR_BIT;
		cfg_invalid++;
	}

	if (raw_mode_is_used(cfg->cmp_mode)) /* additional checks are not needed for the raw mode */
		return -cfg_invalid;

	if (model_mode_is_used(cfg->cmp_mode)) {
		if (cfg->model_value > MAX_MODEL_VALUE) {
			debug_print("Error: selected model_value: %lu is invalid. Largest supported value is: %lu.\n",
				    cfg->model_value, MAX_MODEL_VALUE);
			if (info)
				info->cmp_err |= 1UL << MODEL_VALUE_ERR_BIT;
			cfg_invalid++;
		}
	}

	if (cfg->golomb_par < MIN_ICU_GOLOMB_PAR ||
	    cfg->golomb_par > MAX_ICU_GOLOMB_PAR) {
		debug_print("Error: The selected Golomb parameter: %lu is not supported. The Golomb parameter has to  be between [%lu, %lu].\n",
			    cfg->golomb_par, MIN_ICU_GOLOMB_PAR, MAX_ICU_GOLOMB_PAR);
		if (info)
			info->cmp_err |= 1UL << CMP_PAR_ERR_BIT;
		cfg_invalid++;
	}

	if (cfg->spill < MIN_ICU_SPILL) {
		debug_print("Error: The selected spillover threshold value: %lu is too small. Smallest possible spillover value is: %lu.\n",
			    cfg->spill, MIN_ICU_SPILL);
		if (info)
			info->cmp_err |= 1UL << CMP_PAR_ERR_BIT;
		cfg_invalid++;
	}

	if (cfg->spill > get_max_spill(cfg->golomb_par, cfg->cmp_mode)) {
		debug_print("Error: The selected spillover threshold value: %lu is too large for the selected Golomb parameter: %lu, the largest possible spillover value in the selected compression mode is: %lu.\n",
			    cfg->spill, cfg->golomb_par,
			    get_max_spill(cfg->golomb_par, cfg->cmp_mode));
		if (info)
			info->cmp_err |= 1UL << CMP_PAR_ERR_BIT;
		cfg_invalid++;
	}

#ifdef ADAPTIVE_CHECK_ENA
	/*
	 * ap1_spill and ap2_spill are not used for the icu_compression
	 */

	if (cfg->ap1_spill > get_max_spill(cfg->ap1_golomb_par, cfg->cmp_mode)) {
		if (info)
			info->cmp_err |= 1UL << AP1_CMP_PAR_ERR_BIT;
		cfg_invalid++;
	}

	if (cfg->ap2_spill > get_max_spill(cfg->ap2_golomb_par, cfg->cmp_mode)) {
		if (info)
			info->cmp_err |= 1UL << AP2_CMP_PAR_ERR_BIT;
		cfg_invalid++;
	}
#endif

	if (cfg->round > MAX_ICU_ROUND) {
		debug_print("Error: selected round parameter: %lu is not supported. Largest supported value is: %lu.\n",
			    cfg->round, MAX_ICU_ROUND);
		cfg_invalid++;
	}

	return -(cfg_invalid);
}


/**
 * @brief
 *
 * @param cfg	configuration contains all parameters required for compression
 * @param info	compressor information contains information of an executed
 *		compression (can be NULL)
 *
 * @returns 0 on success, error otherwise
 */


static int set_info(struct cmp_cfg *cfg, struct cmp_info *info)
{
	if (!cfg)
		return -1;

	if (cfg->cmp_mode > UINT8_MAX)
		return -1;

	if (cfg->round > UINT8_MAX)
		return -1;

	if (cfg->model_value > UINT8_MAX)
		return -1;

	if(info) {
		info->cmp_err = 0;
		info->cmp_mode_used = (uint8_t)cfg->cmp_mode;
		info->model_value_used = (uint8_t)cfg->model_value;
		info->round_used = (uint8_t)cfg->round;
		info->spill_used = cfg->spill;
		info->golomb_par_used = cfg->golomb_par;
		info->samples_used = cfg->samples;
		info->cmp_size = 0;
		info->ap1_cmp_size = 0;
		info->ap2_cmp_size = 0;
		info->rdcu_new_model_adr_used = cfg->rdcu_new_model_adr;
		info->rdcu_cmp_adr_used = cfg->rdcu_buffer_adr;
	}
	return 0;
}


/**
 * @brief 1d-differentiating pre-processing and rounding of a uint16_t data buffer
 *
 * @note change the data_buf in-place
 * @note output is I[0] = I[0], I[i] = I[i] - I[i-1], where i is 1,2,..samples-1
 *
 * @param data_buf	pointer to the uint16_t formatted data buffer to process
 * @param samples	amount of data samples in the data buffer
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int diff_16(uint16_t *data_buf, unsigned int samples, unsigned int round)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	lossy_rounding_16(data_buf, samples, round);

	for (i = samples - 1; i > 0; i--) {
		/* possible underflow is intended */
		data_buf[i] = data_buf[i] - data_buf[i-1];
	}
	return 0;
}


/**
 * @brief 1d-differentiating pre-processing and rounding of a uint32_t data buffer
 *
 * @note change the data_buf in-place
 * @note output is I_0 = I_0, I_i = I_i - I_i-1, where i is the array index
 *
 * @param data_buf	pointer to the uint32_t formatted data buffer to process
 * @param samples	amount of data samples in the data_buf buffer
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int diff_32(uint32_t *data_buf, unsigned int samples, unsigned int round)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	lossy_rounding_32(data_buf, samples, round);

	for (i = samples - 1; i > 0; i--) {
		/* possible underflow is intended */
		data_buf[i] = data_buf[i] - data_buf[i-1];
	}
	return 0;
}


/**
 * @brief 1d-differentiating pre-processing and round of a S_FX data buffer
 *
 * @note change the data_buf in-place
 * @note output is I_0 = I_0, I_i = I_i - I_i-1, where i is the array index
 *
 * @param data_buf	pointer to a S_FX data buffer
 * @param samples	amount of data samples in the data buffer
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int diff_S_FX(struct S_FX *data, unsigned int samples, unsigned int
		     round)
{
	size_t i;
	int err;

	if (!samples)
		return 0;

	if (!data)
		return -1;

	err = lossy_rounding_S_FX(data, samples, round);
	if (err)
		return err;

	for (i = samples - 1; i > 0; i--) {
		/* possible underflow is intended */
		data[i] = sub_S_FX(data[i], data[i-1]);
	}
	return 0;
}


/**
 * @brief 1d-differentiating pre-processing and rounding of a S_FX_EFX data buffer
 *
 * @note change the data_buf in-place
 * @note output is I_0 = I_0, I_i = I_i - I_i-1, where i is the array index
 *
 * @param data_buf	pointer to a S_FX_EFX data buffer
 * @param samples	amount of data samples in the data buffer
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int diff_S_FX_EFX(struct S_FX_EFX *data, unsigned int samples, unsigned
			 int round)
{
	size_t i;
	int err;

	if (!samples)
		return 0;

	if (!data)
		return -1;

	err = lossy_rounding_S_FX_EFX(data, samples, round);
	if (err)
		return err;

	for (i = samples - 1; i > 0; i--) {
		/* possible underflow is intended */
		data[i] = sub_S_FX_EFX(data[i], data[i-1]);
	}
	return 0;
}


/**
 * @brief 1d-differentiating pre-processing and rounding of a S_FX_NCOB data buffer
 *
 * @note change the data_buf in-place
 * @note output is I_0 = I_0, I_i = I_i - I_i-1, where i is the array index
 *
 * @param data_buf	pointer to a S_FX_NCOB data buffer
 * @param samples	amount of data samples in the data buffer
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int diff_S_FX_NCOB(struct S_FX_NCOB *data, unsigned int samples, unsigned
			  int round)
{
	size_t i;
	int err;

	if (!samples)
		return 0;

	if (!data)
		return -1;

	err = lossy_rounding_S_FX_NCOB(data, samples, round);
	if (err)
		return err;

	for (i = samples - 1; i > 0; i--) {
		/* possible underflow is intended */
		data[i] = sub_S_FX_NCOB(data[i], data[i-1]);
	}
	return 0;
}


/**
 * @brief 1d-differentiating pre-processing and rounding of a S_FX_EFX_NCOB_ECOB data buffer
 *
 * @note change the data_buf in-place
 * @note output is I_0 = I_0, I_i = I_i - I_i-1, where i is the array index
 *
 * @param data_buf	pointer to a S_FX_EFX_NCOB_ECOB data buffer
 * @param samples	amount of data samples in the data buffer
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int diff_S_FX_EFX_NCOB_ECOB(struct S_FX_EFX_NCOB_ECOB *data, unsigned int
				   samples, unsigned int round)
{
	size_t i;
	int err;

	if (!samples)
		return 0;

	if (!data)
		return -1;

	err = lossy_rounding_S_FX_EFX_NCOB_ECOB(data, samples, round);
	if (err)
		return err;

	for (i = samples - 1; i > 0; i--) {
		/* possible underflow is intended */
		data[i] = sub_S_FX_EFX_NCOB_ECOB(data[i], data[i-1]);
	}

	return 0;
}


/**
 * @brief model pre-processing and rounding of a uint16_t data buffer
 *
 * @note overwrite the data_buf in-place with the result
 * @note update the model_buf in-place
 *
 * @param data_buf	pointer to the uint16_t data buffer to process
 * @param model_buf	pointer to the model buffer of the data to process
 * @param up_model_buf	pointer to the updated model buffer can be NULL
 * @param samples	amount of data samples in the data_buf and model_buf buffer
 * @param model_value	model weighting parameter
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int model_16(uint16_t *data_buf, uint16_t *model_buf, uint16_t *up_model_buf,
		    unsigned int samples, unsigned int model_value, unsigned int round)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	if (!model_buf)
		return -1;

	if (model_value > MAX_MODEL_VALUE)
		return -1;

	if (!up_model_buf)
		up_model_buf = model_buf;

	for (i = 0; i < samples; i++) {
		uint16_t round_input = (uint16_t)round_fwd(data_buf[i], round);
		uint16_t round_model = (uint16_t)round_fwd(model_buf[i], round);
		/* possible underflow is intended */
		data_buf[i] = round_input - round_model; /* TDOO: check if this is the right order */
		/* round back input because for decompression the accurate data
		 * are not available
		 */
		up_model_buf[i] = (uint16_t)cal_up_model(round_inv(round_input, round),
							 model_buf[i], model_value);
	}
	return 0;
}


/**
 * @brief model pre-processing and round_input of a uint32_t data buffer
 *
 * @note overwrite the data_buf in-place with the result
 * @note update the model_buf in-place
 *
 * @param data_buf	pointer to the uint32_t data buffer to process
 * @param model_buf	pointer to the model buffer of the data to process
 * @param samples	amount of data samples in the data_buf and model_buf buffer
 * @param model_value	model weighting parameter
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

static int model_32(uint32_t *data_buf, uint32_t *model_buf, unsigned int samples,
		    unsigned int model_value, unsigned int round)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	if (!model_buf)
		return -1;

	if (model_value > MAX_MODEL_VALUE)
		return -1;

	for (i = 0; i < samples; i++) {
		uint32_t round_input = round_fwd(data_buf[i], round);
		uint32_t round_model = round_fwd(model_buf[i], round);
		/* possible underflow is intended */
		data_buf[i] = round_input - round_model;
		/* round back input because for decompression the accurate data
		 * are not available
		 */
		model_buf[i] = cal_up_model(round_inv(round_input, round),
					    model_buf[i], model_value);
	}
	return 0;
}


/**
 * @brief model pre-processing and round_input of a S_FX data buffer
 *
 * @note overwrite the data_buf in-place with the result
 * @note update the model_buf in-place
 *
 * @param data_buf	pointer to the S_FX data buffer to process
 * @param model_buf	pointer to the model buffer of the data to process
 * @param model_buf	pointer to the updated model buffer (if NULL model_buf
 *	will be overwrite with the updated model)
 * @param samples	amount of data samples in the data_buf and model_buf buffer
 * @param model_value	model weighting parameter
 * @param round		number of bits to round; if zero no rounding takes place
 *
 * @returns 0 on success, error otherwise
 */

int model_S_FX(struct S_FX *data_buf, struct S_FX *model_buf,
		      struct S_FX *up_model_buf, unsigned int samples,
		      unsigned int model_value, unsigned int round)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	if (!model_buf)
		return -1;

	if (model_value > MAX_MODEL_VALUE)
		return -1;

	if (!up_model_buf)  /* overwrite the model buffer if no up_model_buf is set */
		up_model_buf = model_buf;


	for (i = 0; i < samples; i++) {
		struct S_FX round_data = data_buf[i];
		struct S_FX round_model = model_buf[i];
		int err;

		err = lossy_rounding_S_FX(&round_data, 1, round);
		if (err)
			return err;

		err = lossy_rounding_S_FX(&round_model, 1, round);
		if (err)
			return err;

		/* possible underflow is intended */
		data_buf[i] = sub_S_FX(round_data, round_model);

		/* round back input because for decompression the accurate data
		 * are not available
		 */
		err = de_lossy_rounding_S_FX(&round_data, 1, round);
		if (err)
			return err;
		up_model_buf[i] = cal_up_model_S_FX(round_data, model_buf[i],
						    model_value);
	}

	return 0;
}


int pre_process(struct cmp_cfg *cfg)
{
	if (!cfg)
		return -1;

	if (cfg->samples == 0)
		return 0;

	if (!cfg->input_buf)
		return -1;

	switch (cfg->cmp_mode) {
	case MODE_RAW:
	case MODE_RAW_S_FX:
		return 0; /* in raw mode no pre-processing is necessary */
		break;
	case MODE_MODEL_ZERO:
	case MODE_MODEL_MULTI:
		return model_16((uint16_t *)cfg->input_buf, (uint16_t *)cfg->model_buf,
				(uint16_t *)cfg->icu_new_model_buf, cfg->samples,
				cfg->model_value, cfg->round);
		break;
	case MODE_DIFF_ZERO:
	case MODE_DIFF_MULTI:
		return diff_16((uint16_t *)cfg->input_buf, cfg->samples,
			       cfg->round);
		break;
	case MODE_MODEL_ZERO_S_FX:
	case MODE_MODEL_MULTI_S_FX:
		return model_S_FX((struct S_FX *)cfg->input_buf, (struct S_FX *)cfg->model_buf,
				  (struct S_FX *)cfg->icu_new_model_buf, cfg->samples,
				  cfg->model_value, cfg->round);
		break;
	case MODE_DIFF_ZERO_S_FX:
	case MODE_DIFF_MULTI_S_FX:
		return diff_S_FX((struct S_FX *)cfg->input_buf, cfg->samples, cfg->round);
		break;
	case MODE_DIFF_ZERO_S_FX_EFX:
	case MODE_DIFF_MULTI_S_FX_EFX:
		return diff_S_FX_EFX((struct S_FX_EFX *)cfg->input_buf,
				     cfg->samples, cfg->round);
		break;
	case MODE_DIFF_ZERO_S_FX_NCOB:
	case MODE_DIFF_MULTI_S_FX_NCOB:
		return diff_S_FX_NCOB((struct S_FX_NCOB *)cfg->input_buf,
				      cfg->samples, cfg->round);
		break;
	case MODE_DIFF_ZERO_S_FX_EFX_NCOB_ECOB:
	case MODE_DIFF_MULTI_S_FX_EFX_NCOB_ECOB:
		return diff_S_FX_EFX_NCOB_ECOB((struct S_FX_EFX_NCOB_ECOB *)cfg->input_buf,
					       cfg->samples, cfg->round);
		break;
	case MODE_MODEL_ZERO_32:
	case MODE_MODEL_MULTI_32:
	case MODE_MODEL_ZERO_F_FX:
	case MODE_MODEL_MULTI_F_FX:
		return model_32((uint32_t *)cfg->input_buf, (uint32_t *)cfg->model_buf,
				cfg->samples, cfg->model_value, cfg->round);
		break;
	case MODE_DIFF_ZERO_32:
	case MODE_DIFF_MULTI_32:
	case MODE_DIFF_ZERO_F_FX:
	case MODE_DIFF_MULTI_F_FX:
		return diff_32((uint32_t *)cfg->input_buf, cfg->samples, cfg->round);
		break;
	default:
		debug_print("Error: Compression mode not supported.\n");
	}

	return -1;
}


static uint8_t map_to_pos_alg_8(int8_t value_to_map)
{
	if (value_to_map < 0)
		/* NOTE: possible integer overflow is intended */
		return (uint8_t)((-value_to_map) * 2 - 1);
	else
		/* NOTE: possible integer overflow is intended */
		return (uint8_t)(value_to_map * 2);
}

static uint16_t map_to_pos_alg_16(int16_t value_to_map)
{
	if (value_to_map < 0)
		/* NOTE: possible integer overflow is intended */
		return (uint16_t)((-value_to_map) * 2 - 1);
	else
		/* NOTE: possible integer overflow is intended */
		return (uint16_t)(value_to_map * 2);
}

static uint32_t map_to_pos_alg_32(int32_t value_to_map)
{
	if (value_to_map < 0)
		/* NOTE: possible integer overflow is intended */
		return (uint32_t)((-value_to_map) * 2 - 1);
	else
		/* NOTE: possible integer overflow is intended */
		return (uint32_t)(value_to_map * 2);
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range for a 16 bit buffer
 *
 * @note overwrite the data_buf in-place with the result
 *
 * @param data_buf	pointer to the uint16_t data buffer to process
 * @param samples	amount of data samples in the data_buf
 * @param zero_mode_used needs to be set if the zero escape symbol mechanism is used
 *
 * @returns 0 on success, error otherwise
 */

static int map_to_pos_16(uint16_t *data_buf, uint32_t samples, int zero_mode_used)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	for (i = 0; i < samples; i++) {
		data_buf[i] = map_to_pos_alg_16((int16_t)data_buf[i]);
		if (zero_mode_used)
			data_buf[i] += 1;
	}
	return 0;
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range for a 32 bit buffer
 *
 * @note overwrite the data_buf in-place with the result
 *
 * @param data_buf	pointer to the uint32_t data buffer to process
 * @param samples	amount of data samples in the data_buf
 * @param zero_mode_used needs to be set if the zero escape symbol mechanism is used
 *
 * @returns 0 on success, error otherwise
 */

static int map_to_pos_32(uint32_t *data_buf, uint32_t samples, int
			 zero_mode_used)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	for (i = 0; i < samples; i++) {
		data_buf[i] = map_to_pos_alg_32((int32_t)data_buf[i]);
		if (zero_mode_used)
			data_buf[i] += 1;
	}
	return 0;
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range for a S_FX buffer
 *
 * @note overwrite the data_buf in-place with the result
 *
 * @param data_buf	pointer to the S_FX data buffer to process
 * @param samples	amount of data samples in the data_buf
 * @param zero_mode_used needs to be set if the zero escape symbol mechanism is used
 *
 * @returns 0 on success, error otherwise
 */

int map_to_pos_S_FX(struct S_FX *data_buf, uint32_t samples, int
			   zero_mode_used)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	for (i = 0; i < samples; i++) {
		data_buf[i].EXPOSURE_FLAGS =
			map_to_pos_alg_8(data_buf[i].EXPOSURE_FLAGS);
		data_buf[i].FX = map_to_pos_alg_32(data_buf[i].FX);

		if (zero_mode_used) {
			/* data_buf[i].EXPOSURE_FLAGS += 1; */
			data_buf[i].FX += 1;
		}
	}
	return 0;
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range for a S_FX_EFX buffer
 *
 * @note overwrite the data_buf in-place with the result
 *
 * @param data_buf	pointer to the S_FX_EFX data buffer to process
 * @param samples	amount of data samples in the data_buf
 * @param zero_mode_used needs to be set if the zero escape symbol mechanism is used
 *
 * @returns 0 on success, error otherwise
 */

static int map_to_pos_S_FX_EFX(struct S_FX_EFX *data_buf, uint32_t samples, int
			       zero_mode_used)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	for (i = 0; i < samples; i++) {
		data_buf[i].EXPOSURE_FLAGS =
			map_to_pos_alg_8(data_buf[i].EXPOSURE_FLAGS);
		data_buf[i].FX = map_to_pos_alg_32(data_buf[i].FX);
		data_buf[i].EFX = map_to_pos_alg_32(data_buf[i].EFX);

		if (zero_mode_used) {
			/* data_buf[i].EXPOSURE_FLAGS += 1; */
			data_buf[i].FX += 1;
			data_buf[i].EFX += 1;
		}
	}
	return 0;
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range for a S_FX_NCOB buffer
 *
 * @note overwrite the data_buf in-place with the result
 *
 * @param data_buf	pointer to the S_FX_NCOB data buffer to process
 * @param samples	amount of data samples in the data_buf
 * @param zero_mode_used needs to be set if the zero escape symbol mechanism is used
 *
 * @returns 0 on success, error otherwise
 */

static int map_to_pos_S_FX_NCOB(struct S_FX_NCOB *data_buf, uint32_t samples,
				int zero_mode_used)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	for (i = 0; i < samples; i++) {
		data_buf[i].EXPOSURE_FLAGS =
			map_to_pos_alg_8(data_buf[i].EXPOSURE_FLAGS);
		data_buf[i].FX = map_to_pos_alg_32(data_buf[i].FX);
		data_buf[i].NCOB_X = map_to_pos_alg_32(data_buf[i].NCOB_X);
		data_buf[i].NCOB_Y = map_to_pos_alg_32(data_buf[i].NCOB_Y);

		if (zero_mode_used) {
			/* data_buf[i].EXPOSURE_FLAGS += 1; */
			data_buf[i].FX += 1;
			data_buf[i].NCOB_X += 1;
			data_buf[i].NCOB_Y += 1;
		}
	}
	return 0;
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range for a S_FX_EFX_NCOB_ECOB buffer
 *
 * @note overwrite the data_buf in-place with the result
 *
 * @param data_buf	pointer to the S_FX_EFX_NCOB_ECOB data buffer to process
 * @param samples	amount of data samples in the data_buf
 * @param zero_mode_used needs to be set if the zero escape symbol mechanism is used
 *
 * @returns 0 on success, error otherwise
 */

static int map_to_pos_S_FX_EFX_NCOB_ECOB(struct S_FX_EFX_NCOB_ECOB *data_buf,
					 uint32_t samples, int zero_mode_used)
{
	size_t i;

	if (!samples)
		return 0;

	if (!data_buf)
		return -1;

	for (i = 0; i < samples; i++) {
		data_buf[i].EXPOSURE_FLAGS =
			map_to_pos_alg_8(data_buf[i].EXPOSURE_FLAGS);
		data_buf[i].FX = map_to_pos_alg_32(data_buf[i].FX);
		data_buf[i].NCOB_X = map_to_pos_alg_32(data_buf[i].NCOB_X);
		data_buf[i].NCOB_Y = map_to_pos_alg_32(data_buf[i].NCOB_Y);
		data_buf[i].EFX = map_to_pos_alg_32(data_buf[i].EFX);
		data_buf[i].ECOB_X = map_to_pos_alg_32(data_buf[i].ECOB_X);
		data_buf[i].ECOB_Y = map_to_pos_alg_32(data_buf[i].ECOB_Y);

		if (zero_mode_used) {
			/* data_buf[i].EXPOSURE_FLAGS += 1; */
			data_buf[i].FX += 1;
			data_buf[i].NCOB_X += 1;
			data_buf[i].NCOB_Y += 1;
			data_buf[i].EFX += 1;
			data_buf[i].ECOB_X += 1;
			data_buf[i].ECOB_Y += 1;
		}
	}
	return 0;
}


/**
 * @brief map the signed output of the pre-processing stage to a unsigned value
 *	range
 *
 * @note change the data_buf in-place
 *
 * @param data_buf	pointer to the data to process
 * @param buf_len	length of the data to process
 *
 * @returns 0 on success, error otherwise
 */

static int map_to_pos(struct cmp_cfg *cfg)
{
	int zero_mode_used;

	if (!cfg)
		return -1;

	if (cfg->samples == 0)
		return 0;

	if (!cfg->input_buf)
		return -1;

	zero_mode_used = zero_escape_mech_is_used(cfg->cmp_mode);

	switch (cfg->cmp_mode) {
	case MODE_RAW:
	case MODE_RAW_S_FX:
		return 0; /* in raw mode no mapping is necessary */
		break;
	case MODE_MODEL_ZERO:
	case MODE_MODEL_MULTI:
	case MODE_DIFF_ZERO:
	case MODE_DIFF_MULTI:
		return map_to_pos_16((uint16_t *)cfg->input_buf, cfg->samples,
				     zero_mode_used);
		break;
	case MODE_MODEL_ZERO_S_FX:
	case MODE_MODEL_MULTI_S_FX:
	case MODE_DIFF_ZERO_S_FX:
	case MODE_DIFF_MULTI_S_FX:
		return map_to_pos_S_FX((struct S_FX *)cfg->input_buf,
				       cfg->samples, zero_mode_used);
		break;
	case MODE_MODEL_ZERO_S_FX_EFX:
	case MODE_MODEL_MULTI_S_FX_EFX:
	case MODE_DIFF_ZERO_S_FX_EFX:
	case MODE_DIFF_MULTI_S_FX_EFX:
		return map_to_pos_S_FX_EFX((struct S_FX_EFX *)cfg->input_buf,
					   cfg->samples, zero_mode_used);
		break;
	case MODE_MODEL_ZERO_S_FX_NCOB:
	case MODE_MODEL_MULTI_S_FX_NCOB:
	case MODE_DIFF_ZERO_S_FX_NCOB:
	case MODE_DIFF_MULTI_S_FX_NCOB:
		return map_to_pos_S_FX_NCOB((struct S_FX_NCOB *)cfg->input_buf,
					    cfg->samples, zero_mode_used);
		break;
	case MODE_MODEL_ZERO_S_FX_EFX_NCOB_ECOB:
	case MODE_MODEL_MULTI_S_FX_EFX_NCOB_ECOB:
	case MODE_DIFF_ZERO_S_FX_EFX_NCOB_ECOB:
	case MODE_DIFF_MULTI_S_FX_EFX_NCOB_ECOB:
		return map_to_pos_S_FX_EFX_NCOB_ECOB((struct S_FX_EFX_NCOB_ECOB *)
						     cfg->input_buf,
						     cfg->samples,
						     zero_mode_used);
		break;
	case MODE_MODEL_ZERO_32:
	case MODE_MODEL_MULTI_32:
	case MODE_DIFF_ZERO_32:
	case MODE_DIFF_MULTI_32:
	case MODE_MODEL_ZERO_F_FX:
	case MODE_MODEL_MULTI_F_FX:
	case MODE_DIFF_ZERO_F_FX:
	case MODE_DIFF_MULTI_F_FX:
		return map_to_pos_32((uint32_t *)cfg->input_buf, cfg->samples,
				     zero_mode_used);
		break;
	default:
		debug_print("Error: Compression mode not supported.\n");
		break;

	}

	return -1;
}


/**
 * @brief forms the codeword accurate to the Rice code and returns its length
 *
 * @param m		Golomb parameter, only m's which are power of 2 and >0
 *			are allowed!
 * @param log2_m	Rice parameter, is log_2(m) calculate outside function
 *			for better performance
 * @param value		value to be encoded
 * @param cw		address were the encode code word is stored
 *
 * @returns length of the encoded code word in bits
 */

static unsigned int Rice_encoder(unsigned int value, unsigned int m, unsigned
				 int log2_m, unsigned int *cw)
{
	unsigned int g;  /* quotient of value/m */
	unsigned int q;  /* quotient code without ending zero */
	unsigned int r;  /* remainder of value/m */
	unsigned int rl; /* remainder length */

	g = value >> log2_m; /* quotient, number of leading bits */
	q = (1U << g) - 1;    /* prepare the quotient code without ending zero */

	r = value & (m-1);   /* calculate the remainder */
	rl = log2_m + 1;     /* length of the remainder (+1 for the 0 in the
			      * quotient code)
			      */
	*cw = (q << rl) | r; /* put the quotient and remainder code together */

	return rl + g;	      /* calculate the length of the code word */
}


/**
 * @brief forms the codeword accurate to the Golomb code and returns its length
 *
 * @param m		Golomb parameter, only m's which are power of 2 and >0
 *			are allowed!
 * @param log2_m	is log_2(m) calculate outside function for better
 *			performance
 * @param value		value to be encoded
 * @param cw		address were the encode code word is stored
 *
 * @returns length of the encoded code word in bits
 */

static unsigned int Golomb_encoder(unsigned int value, unsigned int m, unsigned int log2_m, unsigned int *cw)
{
	unsigned int len0, b, g, q, lg;
	unsigned int len;
	unsigned int cutoff;

	len0 = log2_m + 1; /* codeword length in group 0 */
	cutoff = (1U << (log2_m+1)) - m; /* members in group 0 */
	if (cutoff == 0) /* for powers of two we fix cutoff = m */
		cutoff = m;

	if (value < cutoff) { /* group 0 */
		*cw = value;
		len = len0;
	} else { /* other groups */
		b = (cutoff << 1); /* form the base codeword */
		g = (value-cutoff)/m; /* this group is which one  */
		lg = len0 + g; /* it has lg remainder bits */
		q = (1U << g) - 1; /* prepare the left side in unary */
		*cw = (q << (len0+1)) + b + (value-cutoff)-g*m; /* composed codeword */
		len = lg + 1; /* length of the codeword */
	}
	return len;
}


typedef unsigned int (*encoder_ptr)(unsigned int, unsigned int, unsigned int,
				    unsigned int*);

static encoder_ptr select_encoder(unsigned int golomb_par)
{
	if (!golomb_par)
		return NULL;

	if (is_a_pow_of_2(golomb_par))
		return &Rice_encoder;
	else
		return &Golomb_encoder;
}


/**
 * @brief    safe (but slow) way to put the value of up to 32 bits into a
 *           bitstream accessed as 32-bit RAM in big endian
 * @param    value      the value to put, it will be masked
 * @param    bitOffset  bit index where the bits will be put, seen from the very
 *			beginning of the bitstream
 * @param    nBits      number of bits to put in the bitstream
 * @param    destAddr   this is the pointer to the beginning of the bitstream
 * @param    dest_len   length of the bitstream buffer (starting at destAddr)
 * @returns  number of bits written, 0 if the number was too big, -1 if the
 *	     destAddr buffer is to small to store the bitstream
 * @note     works in SRAM2
 */

static unsigned int put_n_bits32(unsigned int value, unsigned int bitOffset,
				 unsigned int nBits, unsigned int *destAddr,
				 unsigned int dest_len)
{
	unsigned int *localAddr;
	unsigned int bitsLeft, shiftRight, shiftLeft, localEndPos;
	unsigned int mask;

	/* check if destination buffer is large enough */
	/* TODO: adapt that to the other science products */
	if ((bitOffset + nBits) > (((dest_len+1)/2)*2 * 16)) {
		debug_print("Error: The icu_output_buf buffer is not small to hold the compressed data.\n");
		return -2U;
	}

	/* leave in case of erroneous input */
	if (nBits == 0)
		return 0;
	if (nBits > 32)
		return 0;

	/* separate the bitOffset into word offset (set localAddr pointer) and local bit offset (bitsLeft) */
	localAddr = destAddr + (bitOffset >> 5);
	bitsLeft = bitOffset & 0x1f;

	/* (M) we mask the value first to match its size in nBits */
	/* the calculations can be re-used in the unsegmented code, so we have no overhead */
	shiftRight = 32 - nBits;
	mask = 0xffffffff >> shiftRight;
	value &= mask;

	/* to see if we need to split the value over two words we need the right end position */
	localEndPos = bitsLeft + nBits;

	if (localEndPos <= 32) {
		/*         UNSEGMENTED
		 *
		 *|-----------|XXXXX|----------------|
		 *   bitsLeft    n       bitsRight
		 *
		 *  -> to get the mask:
		 *  shiftRight = bitsLeft + bitsRight = 32 - n
		 *  shiftLeft = bitsRight
		 *
		 */

		/* shiftRight = 32 - nBits; */ /* see (M) above! */
		shiftLeft = shiftRight - bitsLeft;

		/* generate the mask, the bits for the values will be true */
		/* mask = (0xffffffff >> shiftRight) << shiftLeft; */ /* see (M) above! */
		mask <<= shiftLeft;

		/* clear the destination with inverse mask */
		*(localAddr) &= ~mask;

		/* assign the value */
		*(localAddr) |= (value << (32 - localEndPos)); /* NOTE: 32-localEndPos = shiftLeft can be simplified */

	} else {
		/*                             SEGMENTED
		 *
		 *|-----------------------------|XXX| |XX|------------------------------|
		 *          bitsLeft              n1   n2          bitsRight
		 *
		 *  -> to get the mask part 1:
		 *  shiftright = bitsleft
		 *  n1 = n - (bitsleft + n - 32) = 32 - bitsleft
		 *
		 *  -> to get the mask part 2:
		 *  n2 = bitsleft + n - 32
		 *  shiftleft = 32 - n2 = 32 - (bitsleft + n - 32) = 64 - bitsleft - n
		 *
		 */

		unsigned int n2 = bitsLeft + nBits - 32;

		/* part 1: */
		shiftRight = bitsLeft;
		mask = 0xffffffff >> shiftRight;

		/* clear the destination with inverse mask */
		*(localAddr) &= ~mask;

		/* assign the value part 1 */
		*(localAddr) |= (value >> n2);

		/* part 2: */
		/* adjust address */
		localAddr += 1;
		shiftLeft = 64 - bitsLeft - nBits;
		mask = 0xffffffff << shiftLeft;

		/* clear the destination with inverse mask */
		*(localAddr) &= ~mask;

		/* assign the value part 2 */
		*(localAddr) |= (value << (32 - n2));
	}
	return nBits;
}


struct encoder_struct {
	encoder_ptr encoder;
	unsigned int log2_golomb_par; /* pre-calulated for performance increase */
	/* size_t sample_counter; */
	uint32_t cmp_size; /* Compressed data size; measured in bits */
};


static int encode_raw(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	size_t cmp_size_in_bytes;

	if (!cfg)
		return -1;

	if (!cfg->icu_output_buf)
		return -1;

	if (!cfg->input_buf)
		return -1;

	cmp_size_in_bytes = cfg->samples * size_of_a_sample(cfg->cmp_mode);

	enc->cmp_size = cmp_size_in_bytes * CHAR_BIT; /* cmp_size is measured in bits */

	if (cmp_size_in_bytes > cfg->buffer_length * sizeof(uint16_t))
		return -1;

	memcpy(cfg->icu_output_buf, cfg->input_buf, cmp_size_in_bytes);

	return 0;
}


static int encode_raw_16(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	int err;

	err = encode_raw(cfg, enc);
	if (err)
		return err;

#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
	{
		size_t i;
		uint16_t *p = cfg->icu_output_buf;

		for (i = 0; i < cfg->samples; i++)
			cpu_to_be16s(&p[i]);

	}
#endif /* __BYTE_ORDER__ */
	return 0;
}


static int encode_raw_S_FX(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	int err;

	err = encode_raw(cfg, enc);
	if (err)
		return err;

#if defined(__LITTLE_ENDIAN)
	{
		size_t i;
		for (i = 0; i < cfg->samples; i++) {
			struct S_FX *output_buf = cfg->icu_output_buf;
			output_buf[i].FX = cpu_to_be32(output_buf[i].FX);
		}
	}
#endif
	return 0;
}


static int encode_normal(uint32_t value_to_encode, struct cmp_cfg *cfg,
			 struct encoder_struct *enc)
{
	unsigned int code_word;
	unsigned int cw_len;
	int err;

	if (!enc->encoder)
		return -1;

	cw_len = enc->encoder(value_to_encode, cfg->golomb_par,
			      enc->log2_golomb_par, &code_word);

	err = put_n_bits32(code_word, enc->cmp_size, cw_len,
			   cfg->icu_output_buf, cfg->buffer_length);
	if (err <= 0)
		return err;

	enc->cmp_size += cw_len;

	return 0;
}


static int encode_outlier_zero(uint32_t value_to_encode, int max_bits,
			       struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	int err;

	if (max_bits > 32)
		return -1;

	/* use zero as escape symbol */
	encode_normal(0, cfg, enc);

	/* put the data unencoded in the bitstream */
	err = put_n_bits32(value_to_encode, enc->cmp_size, max_bits,
			   cfg->icu_output_buf, cfg->buffer_length);
	if (err <= 0) {
		return err;
	}

	enc->cmp_size += max_bits;

	return 0;
}


static int cal_multi_offset(unsigned int unencoded_data)
{
	if (unencoded_data <= 0x3)
		return 0;
	if (unencoded_data <= 0xF)
		return 1;
	if (unencoded_data <= 0x3F)
		return 2;
	if (unencoded_data <= 0xFF)
		return 3;
	if (unencoded_data <= 0x3FF)
		return 4;
	if (unencoded_data <= 0xFFF)
		return 5;
	if (unencoded_data <= 0x3FFF)
		return 6;
	if (unencoded_data <= 0xFFFF)
		return 7;
	if (unencoded_data <= 0x3FFFF)
		return 8;
	if (unencoded_data <= 0xFFFFF)
		return 9;
	if (unencoded_data <= 0x3FFFFF)
		return 10;
	if (unencoded_data <= 0xFFFFFF)
		return 11;
	if (unencoded_data <= 0x3FFFFFF)
		return 12;
	if (unencoded_data <= 0xFFFFFFF)
		return 13;
	if (unencoded_data <= 0x3FFFFFFF)
		return 14;
	else
		return 15;
}


static int encode_outlier_multi(uint32_t value_to_encode, struct cmp_cfg *cfg,
				struct encoder_struct *enc)
{
	uint32_t unencoded_data;
	unsigned int unencoded_data_len;
	uint32_t escape_sym;
	int escape_sym_offset;
	int err;

	/*
	 * In this mode we put the difference between the data and the spillover
	 * threshold value (unencoded_data) after a encoded escape symbol, which
	 * indicate that the next codeword is unencoded.
	 * We use different escape symbol depended on the size the needed bit of
	 * unencoded data:
	 * 0, 1, 2 bits needed for unencoded data -> escape symbol is spill + 0
	 * 3, 4 bits needed for unencoded data -> escape symbol is spill + 1
	 * ..
	 */
	unencoded_data = value_to_encode - cfg->spill;
	escape_sym_offset = cal_multi_offset(unencoded_data);
	escape_sym  = cfg->spill + escape_sym_offset;
	unencoded_data_len = (escape_sym_offset + 1) * 2;

	/* put the escape symbol in the bitstream */
	encode_normal(escape_sym, cfg, enc);

	/* put the unencoded data in the bitstream */
	err = put_n_bits32(unencoded_data, enc->cmp_size, unencoded_data_len,
			   cfg->icu_output_buf, cfg->buffer_length);
	if (err <= 0)
		return err;

	enc->cmp_size += unencoded_data_len;

	return 0;
}

static int encode_outlier(uint32_t value_to_encode, int bit_len, struct cmp_cfg
			  *cfg, struct encoder_struct *enc)
{
	if (multi_escape_mech_is_used(cfg->cmp_mode))
		return encode_outlier_multi(value_to_encode, cfg, enc);

	if (zero_escape_mech_is_used(cfg->cmp_mode))
		return encode_outlier_zero(value_to_encode, bit_len, cfg, enc);

	return -1;
}


int encode_value(uint32_t value_to_encode, int bit_len, struct cmp_cfg *cfg,
		 struct encoder_struct *enc)
{
	/* 0 is an outlier in case of a zero-escape mechanism, because an
	 * overflow can occur by incrementing by one
	 */
	if (value_to_encode >= cfg->spill ||
	    (zero_escape_mech_is_used(cfg->cmp_mode) && value_to_encode == 0))
		return encode_outlier(value_to_encode, bit_len, cfg, enc);
	else
		return encode_normal(value_to_encode, cfg, enc);
}


static int encode_16(uint16_t *data_to_encode, struct cmp_cfg *cfg,
		     struct encoder_struct *enc)
{
	size_t i;

	if (!cfg)
		return -1;

	if (!cfg->samples)
		return 0;

	if (!data_to_encode)
		return -1;

	if (!enc)
		return -1;


	for (i = 0; i < cfg->samples; i++) {
		int err = encode_value(data_to_encode[i], 16, cfg, enc);
		if (err)
			return err;
	}
	return 0;
}


static int encode_32(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	uint32_t *data_to_encode = cfg->input_buf;
	size_t i;

	for (i = 0; i < cfg->samples; i++) {
		int err = encode_value(data_to_encode[i], 32, cfg, enc);
		if (err)
			return err;
	}
	return 0;
}

static int encode_S_FX(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	struct S_FX *data_to_encode = cfg->input_buf;
	size_t i;
	struct cmp_cfg cfg_exp_flag = *cfg;
	struct encoder_struct enc_exp_flag = *enc;

	int del_cnt=0;
	static int del_cnt_old=UINT8_MAX;

	cfg_exp_flag.golomb_par = GOLOMB_PAR_EXPOSURE_FLAGS;
	enc_exp_flag.log2_golomb_par = ilog_2(GOLOMB_PAR_EXPOSURE_FLAGS);

	for (i = 0; i < cfg->samples; i++) {
		int err;

		/* err = encode_value(data_to_encode[i].EXPOSURE_FLAGS, 8, &cfg_exp_flag, enc); */
		err = encode_normal(data_to_encode[i].EXPOSURE_FLAGS, &cfg_exp_flag, &enc_exp_flag);
		if (err)
			return err;
		enc->cmp_size = enc_exp_flag.cmp_size;

		enc->log2_golomb_par = ilog_2(cfg->golomb_par);
		err = encode_value(data_to_encode[i].FX, 32, cfg, enc);
		if (err)
			return err;

		enc_exp_flag.cmp_size = enc->cmp_size;
		if (data_to_encode[i].FX >= cfg->spill || (zero_escape_mech_is_used(cfg->cmp_mode) && data_to_encode[i].FX == 0))
			del_cnt++;
	}
	if (del_cnt != del_cnt_old) {
		del_cnt_old = del_cnt;
		/* printf("%.1f%% ", (double)del_cnt/cfg->samples*100.); */
		/* printf("outlier: %i\n", del_cnt); */
	}

	return 0;
}


static int encode_S_FX_EFX(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	struct S_FX_EFX *data_to_encode = cfg->input_buf;
	size_t i;

	for (i = 0; i < cfg->samples; i++) {
		int err;

		err = encode_value(data_to_encode[i].EXPOSURE_FLAGS, 8, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].FX, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].EFX, 32, cfg, enc);
		if (err)
			return err;
	}
	return 0;
}


static int encode_S_FX_NCOB(struct cmp_cfg *cfg, struct encoder_struct *enc)
{
	struct S_FX_NCOB *data_to_encode = cfg->input_buf;
	size_t i;

	for (i = 0; i < cfg->samples; i++) {
		int err;

		err = encode_value(data_to_encode[i].EXPOSURE_FLAGS, 8, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].FX, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].NCOB_X, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].NCOB_Y, 32, cfg, enc);
		if (err)
			return err;
	}
	return 0;
}


static int encode_S_FX_EFX_NCOB_ECOB(struct cmp_cfg *cfg, struct encoder_struct
				     *enc)
{
	struct S_FX_EFX_NCOB_ECOB *data_to_encode = cfg->input_buf;
	size_t i;

	for (i = 0; i < cfg->samples; i++) {
		int err;

		err = encode_value(data_to_encode[i].EXPOSURE_FLAGS, 8, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].FX, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].NCOB_X, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].NCOB_Y, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].EFX, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].ECOB_X, 32, cfg, enc);
		if (err)
			return err;

		err = encode_value(data_to_encode[i].ECOB_Y, 32, cfg, enc);
		if (err)
			return err;
	}
	return 0;
}


static int encode_data(struct cmp_cfg *cfg, struct cmp_info *info)
{
	/* size_t i; */
	struct encoder_struct enc;
	int n_pad_bits;
	int n_bits = 0;
	int err;

	enc.encoder = select_encoder(cfg->golomb_par);
	enc.log2_golomb_par = ilog_2(cfg->golomb_par);
	enc.cmp_size = 0;

	switch (cfg->cmp_mode) {
	case MODE_RAW:
		err = encode_raw_16(cfg, &enc);
		break;
	case MODE_MODEL_ZERO:
	case MODE_MODEL_MULTI:
	case MODE_DIFF_ZERO:
	case MODE_DIFF_MULTI:
		err = encode_16((uint16_t *)cfg->input_buf, cfg, &enc);
		break;
	case MODE_RAW_S_FX:
		err = encode_raw_S_FX(cfg, &enc);
		break;
	case MODE_MODEL_ZERO_S_FX:
	case MODE_MODEL_MULTI_S_FX:
	case MODE_DIFF_ZERO_S_FX:
	case MODE_DIFF_MULTI_S_FX:
		err = encode_S_FX(cfg, &enc);
		break;
	case MODE_MODEL_ZERO_S_FX_EFX:
	case MODE_MODEL_MULTI_S_FX_EFX:
	case MODE_DIFF_ZERO_S_FX_EFX:
	case MODE_DIFF_MULTI_S_FX_EFX:
		err = encode_S_FX_EFX(cfg, &enc);
		break;
	case MODE_MODEL_ZERO_S_FX_NCOB:
	case MODE_MODEL_MULTI_S_FX_NCOB:
	case MODE_DIFF_ZERO_S_FX_NCOB:
	case MODE_DIFF_MULTI_S_FX_NCOB:
		err = encode_S_FX_NCOB(cfg, &enc);
		break;
	case MODE_MODEL_ZERO_S_FX_EFX_NCOB_ECOB:
	case MODE_MODEL_MULTI_S_FX_EFX_NCOB_ECOB:
	case MODE_DIFF_ZERO_S_FX_EFX_NCOB_ECOB:
	case MODE_DIFF_MULTI_S_FX_EFX_NCOB_ECOB:
		err = encode_S_FX_EFX_NCOB_ECOB(cfg, &enc);
		break;
	case MODE_MODEL_ZERO_32:
	case MODE_MODEL_MULTI_32:
	case MODE_DIFF_ZERO_32:
	case MODE_DIFF_MULTI_32:
	case MODE_MODEL_ZERO_F_FX:
	case MODE_MODEL_MULTI_F_FX:
	case MODE_DIFF_ZERO_F_FX:
	case MODE_DIFF_MULTI_F_FX:
		err = encode_32(cfg, &enc);
		break;
	default:
		debug_print("Error: Compression mode not supported.\n");
		return -1;
		break;
	}

	if (err == -2) {
		/* the icu_output_buf is to small to store the whole bitstream */
		info->cmp_err |= 1UL << SMALL_BUFFER_ERR_BIT; /* set small buffer error */
		return err;
	}
	if (info)
		info->cmp_size = enc.cmp_size;

	/* pad the bitstream with zeros */
	/* TODO: make a function of this */
	if (!raw_mode_is_used(cfg->cmp_mode) && enc.cmp_size) {
		n_pad_bits = 32U - (enc.cmp_size & 0x1f);
		if (n_pad_bits < 32) {
			n_bits = put_n_bits32(0, enc.cmp_size, n_pad_bits,
					 cfg->icu_output_buf, cfg->buffer_length);
			if (n_bits <= 0) {
				/* the icu_output_buf is to small to store the whole bitstream */
				if (info) {
					info->cmp_err |= 1UL << SMALL_BUFFER_ERR_BIT; /* set small buffer error */
					info->cmp_size = 0;
				}
				return -2;
			}
		}
#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
		{
			size_t i;
			uint32_t *p = (uint32_t *)cfg->icu_output_buf;
			for (i = 0; i < (info->cmp_size + n_bits)/32; i++)
				cpu_to_be32s(&p[i]);
		}
#endif /*__BYTE_ORDER__ */

	}

	return 0;
}


int icu_compress_data(struct cmp_cfg *cfg, struct cmp_info *info)
{
	int err;

	err = set_info(cfg, info);
	if (err)
		return err;

	err = icu_cmp_cfg_valid(cfg, info);
	if (err)
		return err;

	err = pre_process(cfg);
	if (err)
		return err;

	err = map_to_pos(cfg);
	if (err)
		return err;

	err = encode_data(cfg, info);
	if (err)
		return err;

	return 0;
}
