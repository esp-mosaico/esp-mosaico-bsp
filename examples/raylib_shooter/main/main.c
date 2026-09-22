// SPDX-License-Identifier: Apache-2.0
#include "esp_check.h"
#include "mosaico_game_app.h"
#include "shooter_app.h"

void app_main(void)
{
    ESP_ERROR_CHECK(mosaico_game_app_run(shooter_app_config()));
}
