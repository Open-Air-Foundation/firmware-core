/*
 * SPDX-FileCopyrightText: 2017-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_efuse.h"
#include <assert.h>
#include "esp_efuse_custom_table.h"

// md5_digest_table 167bae548cc4ab06ec365ffb696fe95d
// This file was generated from the file esp_efuse_custom_table.csv. DO NOT CHANGE THIS FILE MANUALLY.
// If you want to change some fields, you need to change esp_efuse_custom_table.csv file
// then run `efuse_common_table` or `efuse_custom_table` command it will generate this file.
// To show efuse_table run the command 'show_efuse_table'.

static const esp_efuse_desc_t USER_DATA_GO_BOARD_ID[] = {
    {EFUSE_BLK3, 0, 8}, // Go board ID: 0=legacy V1 1=V1 2=V2,
};

const esp_efuse_desc_t *ESP_EFUSE_USER_DATA_GO_BOARD_ID[] = {
    &USER_DATA_GO_BOARD_ID[0], // Go board ID: 0=legacy V1 1=V1 2=V2
    NULL};
