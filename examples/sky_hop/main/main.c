// SPDX-License-Identifier: Apache-2.0
#include "esp_check.h"
#include "mosaico_game_app.h"
#include "sky_hop_app.h"

void app_main(void)
{
    ESP_ERROR_CHECK(mosaico_game_app_run(sky_hop_app_config()));
}
