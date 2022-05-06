// Copyright 2022 XMOS LIMITED.
// This Software is subject to the terms of the XMOS Public Licence: Version 1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "vnr_features_api.h"

static vnr_input_state_t vnr_input_state;
static vnr_feature_state_t vnr_feature_state;
void test_init()
{
    vnr_input_state_init(&vnr_input_state);
    vnr_feature_state_init(&vnr_feature_state);
}

void test(int32_t *output, int32_t *input)
{
    int32_t DWORD_ALIGNED input_frame[VNR_PROC_FRAME_LENGTH + VNR_FFT_PADDING];
    bfp_complex_s32_t X;
    vnr_form_input_frame(&vnr_input_state, &X, input_frame, input);

    bfp_s32_t feature_patch;
    int32_t feature_patch_data[VNR_PATCH_WIDTH*VNR_MEL_FILTERS];
    vnr_extract_features(&vnr_feature_state, &feature_patch, feature_patch_data, &X);

    memcpy(output, &feature_patch.exp, sizeof(int32_t));
    memcpy(&output[1], feature_patch.data, feature_patch.length * sizeof(int32_t));
}
