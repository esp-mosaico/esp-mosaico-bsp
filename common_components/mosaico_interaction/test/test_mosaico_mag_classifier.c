/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaico_mag_classifier.h"

#include "unity.h"

typedef struct {
    int16_t left_x;
    int16_t left_y;
    int16_t left_z;
    int16_t right_x;
    int16_t right_y;
    mosaico_edge_mask_t expected;
} calibration_case_t;

static mosaico_mag_sample_t sample_from_case(const calibration_case_t *test_case)
{
    return (mosaico_mag_sample_t) {
        .right = {
            .valid = true,
            .x = test_case->right_x,
            .y = test_case->right_y,
        },
        .left = {
            .valid = true,
            .x = test_case->left_x,
            .y = test_case->left_y,
            .z = test_case->left_z,
        },
        .timestamp_ms = 100,
    };
}

static mosaico_mag_classification_t classify_stable_with_baseline_axes(
    const calibration_case_t *test_case,
    int16_t baseline_right_y,
    int16_t baseline_right_z,
    int16_t sample_right_z)
{
    mosaico_mag_classifier_t classifier;
    TEST_ESP_OK(mosaico_mag_classifier_init(
        &classifier, &MOSAICO_MAG_CALIBRATION_S31_V1));

    mosaico_mag_classification_t result = {0};
    const mosaico_mag_sample_t baseline = {
        .right = {
            .valid = true,
            .y = baseline_right_y,
            .z = baseline_right_z,
        },
        .left = {.valid = true},
    };
    for (uint8_t i = 0; i < MOSAICO_MAG_CALIBRATION_S31_V1.filter_samples; ++i) {
        TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &baseline, &result));
    }
    TEST_ASSERT_TRUE(classifier.baseline_ready);
    TEST_ASSERT_FALSE(result.valid);

    mosaico_mag_sample_t sample = sample_from_case(test_case);
    sample.right.z = sample_right_z;
    for (uint8_t i = 0; i < MOSAICO_MAG_CALIBRATION_S31_V1.filter_samples; ++i) {
        TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &sample, &result));
    }
    return result;
}

static mosaico_mag_classification_t classify_stable_with_baseline(
    const calibration_case_t *test_case,
    int16_t baseline_right_y)
{
    return classify_stable_with_baseline_axes(
        test_case, baseline_right_y, 0, 0);
}

static mosaico_mag_classification_t classify_stable(const calibration_case_t *test_case)
{
    return classify_stable_with_baseline(test_case, -41);
}

TEST_CASE("S31 V1 calibration classifies cardinal capture centroids", "[mosaico_mag]")
{
    static const calibration_case_t cases[] = {
        {-1198, -32768, 484,  -731,  -14, 0},
        /* ACM4 baseline: its high left Y must not look like bottom. */
        {-1179, 1229, 575, -613, -41, 0},
        /* Slow right approach must not pass through weak bottom or top bands. */
        {-1198, 1188, 524, -832, 164, 0},
        {-1184, 1225, 538, -881, -286, 0},
        /* ACM4 slow left approach must not fabricate top or bottom edges. */
        {-32768, 1034, 570, -499, -76, MOSAICO_EDGE_MASK_LEFT},
        {-32768,  843, 694, -431, -94, MOSAICO_EDGE_MASK_LEFT},
        {-32768, -32768, 735, -452,  -18, MOSAICO_EDGE_MASK_LEFT},
        {-984, -32768, 480, -32768, -72, MOSAICO_EDGE_MASK_RIGHT},
        {-12289, -32768, 473, -671, -388, MOSAICO_EDGE_MASK_TOP},
        /* ACM4 top shifted toward left: X/Y saturate, but Z rejects false left. */
        {-32768, -32768, 560, -560, -404, MOSAICO_EDGE_MASK_TOP},
        {-1063, 742, 438, -691, 305, MOSAICO_EDGE_MASK_BOTTOM},
        {-1026, 909, 446, -661, -32,
         MOSAICO_EDGE_MASK_TOP | MOSAICO_EDGE_MASK_BOTTOM},
        {-32768, -32768, 743, -388, -390,
         MOSAICO_EDGE_MASK_LEFT | MOSAICO_EDGE_MASK_TOP},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const mosaico_mag_classification_t result = classify_stable(&cases[i]);
        TEST_ASSERT_TRUE(result.valid);
        TEST_ASSERT_EQUAL_UINT8(cases[i].expected, result.edge_mask);
    }
}

TEST_CASE("S31 V1 baseline deltas classify both measured devices", "[mosaico_mag]")
{
    static const struct {
        int16_t baseline_right_y;
        calibration_case_t sample;
    } cases[] = {
        {38,  {-1149, -32768, 589,  -644, -308, MOSAICO_EDGE_MASK_TOP}},
        {38,  { -927,   1213, 577, -32768,   45, MOSAICO_EDGE_MASK_RIGHT}},
        {38,  { -885,    663, 586,  -548,  333, MOSAICO_EDGE_MASK_BOTTOM}},
        {38,  {-32768, -32768, 934,  -446,   25, MOSAICO_EDGE_MASK_LEFT}},
        {183, { -678, -32768, 283,  -155, -191, MOSAICO_EDGE_MASK_TOP}},
        {183, { -353,   1185, 282, -32768,  169, MOSAICO_EDGE_MASK_RIGHT}},
        {183, { -549,    601, 347,  -302,  472, MOSAICO_EDGE_MASK_BOTTOM}},
        {183, {-32768,   1274, 729,   125,  187, MOSAICO_EDGE_MASK_LEFT}},
        /* Device 30eda0f40c78 L-shape captures at 0 mm and 0 degrees. */
        {72,  { -976, -32768, 553, -32768, -272,
                MOSAICO_EDGE_MASK_TOP | MOSAICO_EDGE_MASK_RIGHT}},
        {72,  {-32768, -32768, 892,   -399, -347,
                MOSAICO_EDGE_MASK_LEFT | MOSAICO_EDGE_MASK_TOP}},
        {45,  { -694,    628, 537, -32768,  353,
                MOSAICO_EDGE_MASK_RIGHT | MOSAICO_EDGE_MASK_BOTTOM}},
        {45,  {-32768,    738, 980,   -307,  312,
                MOSAICO_EDGE_MASK_BOTTOM | MOSAICO_EDGE_MASK_LEFT}},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const mosaico_mag_classification_t result = classify_stable_with_baseline(
            &cases[i].sample, cases[i].baseline_right_y);
        TEST_ASSERT_TRUE(result.valid);
        TEST_ASSERT_EQUAL_UINT8(cases[i].sample.expected, result.edge_mask);
    }
}

TEST_CASE("S31 V1 separates vertical pairs from bottom transients", "[mosaico_mag]")
{
    static const struct {
        int16_t baseline_right_y;
        int16_t baseline_right_z;
        int16_t sample_right_z;
        calibration_case_t sample;
    } cases[] = {
        {49, 333, 414, {
            .left_x = -957, .left_y = 849, .left_z = 595,
            .right_x = -619, .right_y = 21,
            .expected = MOSAICO_EDGE_MASK_TOP | MOSAICO_EDGE_MASK_BOTTOM,
        }},
        {-74, 245, 264, {
            .left_x = -1178, .left_y = 1025, .left_z = 523,
            .right_x = -596, .right_y = 28,
            .expected = MOSAICO_EDGE_MASK_BOTTOM,
        }},
        {-74, 245, 256, {
            .left_x = -1242, .left_y = 1033, .left_z = 519,
            .right_x = -599, .right_y = 44,
            .expected = MOSAICO_EDGE_MASK_BOTTOM,
        }},
        {49, 333, 391, {
            .left_x = -986, .left_y = 633, .left_z = 586,
            .right_x = -674, .right_y = 371,
            .expected = MOSAICO_EDGE_MASK_BOTTOM,
        }},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const mosaico_mag_classification_t result =
            classify_stable_with_baseline_axes(
                &cases[i].sample, cases[i].baseline_right_y,
                cases[i].baseline_right_z, cases[i].sample_right_z);
        TEST_ASSERT_TRUE(result.valid);
        TEST_ASSERT_EQUAL_UINT8(cases[i].sample.expected, result.edge_mask);
    }
}

TEST_CASE("S31 V1 rejects contaminated startup windows", "[mosaico_mag]")
{
    mosaico_mag_classifier_t classifier;
    TEST_ESP_OK(mosaico_mag_classifier_init(
        &classifier, &MOSAICO_MAG_CALIBRATION_S31_V1));
    mosaico_mag_classification_t result = {0};
    const calibration_case_t contaminated_bottom =
        {-885, 663, 586, -548, 333, MOSAICO_EDGE_MASK_BOTTOM};
    const mosaico_mag_sample_t contaminated = sample_from_case(&contaminated_bottom);

    for (int attempt = 0; attempt < 3; ++attempt) {
        for (uint8_t i = 0; i < MOSAICO_MAG_CALIBRATION_S31_V1.filter_samples; ++i) {
            TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &contaminated, &result));
        }
    }
    TEST_ASSERT_FALSE(classifier.baseline_ready);
    TEST_ASSERT_EQUAL(MOSAICO_MAG_CALIBRATION_FAILED, classifier.calibration_state);

    const mosaico_mag_sample_t clean = {
        .right = {.valid = true, .y = 50},
        .left = {.valid = true},
    };
    for (uint8_t i = 0; i < MOSAICO_MAG_CALIBRATION_S31_V1.filter_samples; ++i) {
        TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &clean, &result));
    }
    TEST_ASSERT_TRUE(classifier.baseline_ready);
    TEST_ASSERT_EQUAL(MOSAICO_MAG_CALIBRATION_READY, classifier.calibration_state);
    TEST_ASSERT_EQUAL_INT32(50, classifier.baseline_right_y);
}

TEST_CASE("S31 V1 keeps the known L plus U ambiguity visible", "[mosaico_mag]")
{
    const calibration_case_t ambiguous = {
        -32768, 723, 710, -478, 284,
        MOSAICO_EDGE_MASK_LEFT | MOSAICO_EDGE_MASK_BOTTOM,
    };
    const mosaico_mag_classification_t result = classify_stable(&ambiguous);
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_EQUAL_UINT8(ambiguous.expected, result.edge_mask);
}

typedef struct {
    mosaico_mag_presence_event_t events[8];
    size_t count;
} presence_events_t;

static void presence_event_callback(const mosaico_mag_presence_event_t *event, void *user_ctx)
{
    presence_events_t *events = user_ctx;
    TEST_ASSERT_LESS_THAN(sizeof(events->events) / sizeof(events->events[0]), events->count);
    events->events[events->count++] = *event;
}

TEST_CASE("presence tracker debounces multiple edges independently", "[mosaico_mag]")
{
    presence_events_t events = {0};
    mosaico_mag_presence_tracker_t tracker;
    TEST_ESP_OK(mosaico_mag_presence_tracker_init(
        &tracker, NULL, presence_event_callback, &events));
    mosaico_mag_classification_t result = {
        .valid = true,
        .edge_mask = MOSAICO_EDGE_MASK_LEFT | MOSAICO_EDGE_MASK_TOP,
        .timestamp_ms = 100,
    };

    for (int i = 0; i < 3; ++i) {
        TEST_ESP_OK(mosaico_mag_presence_tracker_process(&tracker, &result));
    }
    TEST_ASSERT_EQUAL_UINT8(2, events.count);
    TEST_ASSERT_EQUAL_UINT8(
        MOSAICO_EDGE_MASK_LEFT | MOSAICO_EDGE_MASK_TOP, tracker.attached_mask);

    result.valid = false;
    result.edge_mask = 0;
    for (int i = 0; i < 3; ++i) {
        TEST_ESP_OK(mosaico_mag_presence_tracker_process(&tracker, &result));
    }
    TEST_ASSERT_EQUAL_UINT8(4, events.count);
    TEST_ASSERT_EQUAL_UINT8(0, tracker.attached_mask);

    result.valid = true;
    for (int i = 0; i < 3; ++i) {
        TEST_ESP_OK(mosaico_mag_presence_tracker_process(&tracker, &result));
    }
    TEST_ASSERT_EQUAL_UINT8(4, events.count);
    TEST_ASSERT_EQUAL_UINT8(0, tracker.attached_mask);
}

TEST_CASE("invalid sample clears classifier history", "[mosaico_mag]")
{
    mosaico_mag_classifier_t classifier;
    TEST_ESP_OK(mosaico_mag_classifier_init(
        &classifier, &MOSAICO_MAG_CALIBRATION_S31_V1));
    mosaico_mag_sample_t sample = {
        .right = {.valid = true, .y = 183},
        .left = {.valid = true},
    };
    mosaico_mag_classification_t result;
    for (uint8_t i = 0; i < MOSAICO_MAG_CALIBRATION_S31_V1.filter_samples; ++i) {
        TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &sample, &result));
    }
    TEST_ASSERT_TRUE(classifier.baseline_ready);

    sample.right.y = -191;
    for (int i = 0; i < 7; ++i) {
        TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &sample, &result));
    }
    sample.left.valid = false;
    TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &sample, &result));
    TEST_ASSERT_FALSE(result.valid);
    TEST_ASSERT_EQUAL_UINT8(0, classifier.sample_count);
    TEST_ASSERT_TRUE(classifier.baseline_ready);

    sample.left.valid = true;
    for (uint8_t i = 0; i < MOSAICO_MAG_CALIBRATION_S31_V1.filter_samples; ++i) {
        TEST_ESP_OK(mosaico_mag_classifier_process(&classifier, &sample, &result));
    }
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_EQUAL_UINT8(MOSAICO_EDGE_MASK_TOP, result.edge_mask);
}
