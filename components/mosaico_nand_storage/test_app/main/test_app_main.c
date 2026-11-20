/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "esp_newlib.h"
#include "mosaico_nand_storage.h"
#include "nand_test_common.h"
#include "unity.h"
#include "unity_test_utils.h"
#include "unity_test_utils_memory.h"

void setUp(void)
{
    unity_utils_record_free_mem();
}

void tearDown(void)
{
    (void)mosaico_nand_storage_unmount();
    nand_test_cleanup();
    esp_reent_cleanup();
    unity_utils_evaluate_leaks_direct(512);
}

void app_main(void)
{
    printf("\nMosaico NAND storage test application\n");
    printf("WARNING: [destructive] tests erase NAND blocks or format the filesystem.\n");
    printf("Run [auto] for the bounded automated suite; use the menu for manual endurance and power tests.\n\n");
    unity_run_menu();
}
