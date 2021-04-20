/**
 * @file   icu_cmp.h
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
 */

#ifndef _ICU_CMP_H_
#define _ICU_CMP_H_

#include "../include/cmp_support.h"

int icu_compress_data(struct cmp_cfg *cfg, struct cmp_info *info);

#endif /* _ICU_CMP_H_ */
