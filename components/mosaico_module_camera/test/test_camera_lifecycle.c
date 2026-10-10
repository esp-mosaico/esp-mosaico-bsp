// SPDX-License-Identifier: Apache-2.0

#include "esp_check.h"
#include "mosaico_module_camera.h"
#include "unity.h"

/* Hardware tests: LEFT camera, no other camera consumer. Each case releases
 * its frame/handle before Unity assertions so a failure does not leak a loan. */
typedef struct {
    mosaico_camera_handle_t camera;
    mosaico_camera_frame_t frame;
    bool borrowed;
} camera_fixture_t;

static const char *TAG = "camera_lifecycle_test";

static esp_err_t fixture_start(camera_fixture_t *fixture)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.allow_unidentified = true;
    ESP_RETURN_ON_ERROR(mosaico_camera_new(&config, &fixture->camera), TAG, "create camera");
    ESP_RETURN_ON_ERROR(mosaico_camera_open(fixture->camera), TAG, "open camera");
    return mosaico_camera_start_stream(fixture->camera);
}

static esp_err_t fixture_return(camera_fixture_t *fixture)
{
    if (fixture->borrowed) {
        ESP_RETURN_ON_ERROR(mosaico_camera_return_frame(fixture->camera, &fixture->frame),
                            TAG, "return frame");
        fixture->borrowed = false;
    }
    return ESP_OK;
}

static esp_err_t fixture_capture(camera_fixture_t *fixture)
{
    ESP_RETURN_ON_ERROR(mosaico_camera_get_frame(fixture->camera, &fixture->frame),
                        TAG, "capture frame");
    fixture->borrowed = true;
    ESP_RETURN_ON_FALSE(fixture->frame.data && fixture->frame.size > 0,
                        ESP_FAIL, TAG, "empty frame");
    return ESP_OK;
}

static esp_err_t fixture_delete(camera_fixture_t *fixture)
{
    ESP_RETURN_ON_ERROR(fixture_return(fixture), TAG, "release borrowed frame");
    if (fixture->camera) {
        ESP_RETURN_ON_ERROR(mosaico_camera_del(fixture->camera), TAG, "delete camera");
        fixture->camera = NULL;
    }
    return ESP_OK;
}

static esp_err_t stop_and_resume(camera_fixture_t *fixture)
{
    ESP_RETURN_ON_ERROR(fixture_start(fixture), TAG, "start");
    ESP_RETURN_ON_ERROR(fixture_capture(fixture), TAG, "initial capture");
    ESP_RETURN_ON_ERROR(fixture_return(fixture), TAG, "initial return");
    ESP_RETURN_ON_ERROR(mosaico_camera_stop_stream(fixture->camera), TAG, "stop");
    ESP_RETURN_ON_ERROR(mosaico_camera_stop_stream(fixture->camera), TAG, "stop again");
    mosaico_camera_pipeline_stats_t stats;
    ESP_RETURN_ON_ERROR(mosaico_camera_get_pipeline_stats(fixture->camera, &stats), TAG, "stats");
    ESP_RETURN_ON_FALSE(!stats.streaming && stats.outstanding_count == 0,
                        ESP_FAIL, TAG, "stream not stopped");
    ESP_RETURN_ON_ERROR(mosaico_camera_start_stream(fixture->camera), TAG, "resume");
    ESP_RETURN_ON_ERROR(fixture_capture(fixture), TAG, "capture after resume");
    ESP_RETURN_ON_ERROR(fixture_return(fixture), TAG, "return after resume");
    ESP_RETURN_ON_ERROR(mosaico_camera_close(fixture->camera), TAG, "close");
    ESP_RETURN_ON_ERROR(mosaico_camera_open(fixture->camera), TAG, "reopen");
    ESP_RETURN_ON_ERROR(mosaico_camera_start_stream(fixture->camera), TAG, "start after reopen");
    return fixture_capture(fixture);
}

static esp_err_t delete_and_recreate(camera_fixture_t *fixture)
{
    ESP_RETURN_ON_ERROR(fixture_start(fixture), TAG, "start");
    ESP_RETURN_ON_ERROR(fixture_capture(fixture), TAG, "initial capture");
    ESP_RETURN_ON_ERROR(fixture_delete(fixture), TAG, "release module");
    ESP_RETURN_ON_ERROR(fixture_start(fixture), TAG, "reclaim module");
    return fixture_capture(fixture);
}

static esp_err_t reject_borrowed_frame(camera_fixture_t *fixture)
{
    ESP_RETURN_ON_ERROR(fixture_start(fixture), TAG, "start");
    ESP_RETURN_ON_ERROR(fixture_capture(fixture), TAG, "borrow frame");
    ESP_RETURN_ON_FALSE(mosaico_camera_stop_stream(fixture->camera) == ESP_ERR_INVALID_STATE,
                        ESP_FAIL, TAG, "stop must reject borrowed frame");
    ESP_RETURN_ON_FALSE(mosaico_camera_close(fixture->camera) == ESP_ERR_INVALID_STATE,
                        ESP_FAIL, TAG, "close must reject borrowed frame");
    ESP_RETURN_ON_FALSE(mosaico_camera_restart(fixture->camera) == ESP_ERR_INVALID_STATE,
                        ESP_FAIL, TAG, "restart must reject borrowed frame");
    esp_err_t ret = mosaico_camera_del(fixture->camera);
    if (ret == ESP_OK) {
        /* A broken driver may have freed the loan: never use that handle again. */
        fixture->camera = NULL;
        fixture->borrowed = false;
        return ESP_FAIL;
    }
    ESP_RETURN_ON_FALSE(ret == ESP_ERR_INVALID_STATE, ESP_FAIL, TAG, "unexpected delete error");
    mosaico_camera_pipeline_stats_t stats;
    ESP_RETURN_ON_ERROR(mosaico_camera_get_pipeline_stats(fixture->camera, &stats), TAG, "stats");
    ESP_RETURN_ON_FALSE(stats.streaming && stats.outstanding_count == 1,
                        ESP_FAIL, TAG, "rejected lifecycle call changed frame ownership");
    ESP_RETURN_ON_ERROR(fixture_return(fixture), TAG, "return preserved loan");
    ESP_RETURN_ON_ERROR(mosaico_camera_restart(fixture->camera), TAG, "restart after return");
    return fixture_capture(fixture);
}

static void run_lifecycle_case(esp_err_t (*scenario)(camera_fixture_t *))
{
    camera_fixture_t fixture = {0};
    const esp_err_t result = scenario(&fixture);
    const esp_err_t cleanup = fixture_delete(&fixture);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, result, "camera lifecycle scenario failed; see log");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, cleanup, "camera fixture cleanup failed");
}

TEST_CASE("camera resumes capture after stop and close", "[mosaico_camera][hardware]")
{
    run_lifecycle_case(stop_and_resume);
}

TEST_CASE("camera releases its module lease for a new instance", "[mosaico_camera][hardware]")
{
    run_lifecycle_case(delete_and_recreate);
}

TEST_CASE("camera lifecycle operations preserve borrowed frames", "[mosaico_camera][hardware]")
{
    run_lifecycle_case(reject_borrowed_frame);
}
