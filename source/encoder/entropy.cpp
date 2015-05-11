/*****************************************************************************
* Copyright (C) 2013 x265 project
*
* Authors: Steve Borho <steve@borho.org>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
*
* This program is also available under a commercial proprietary license.
* For more information, contact us at license @ x265.com.
*****************************************************************************/

#include "common.h"
#include "framedata.h"
#include "scalinglist.h"
#include "quant.h"
#include "contexts.h"
#include "picyuv.h"

#include "sao.h"
#include "entropy.h"

#define CU_DQP_TU_CMAX 5 // max number bins for truncated unary
#define CU_DQP_EG_k    0 // exp-golomb order
#define START_VALUE    8 // start value for dpcm mode

static const uint32_t g_puOffset[8] = { 0, 8, 4, 4, 2, 10, 1, 5 };

namespace x265 {

Entropy::Entropy()
{
    markValid();
    m_fracBits = 0;
    m_pad = 0;
    X265_CHECK(sizeof(m_contextState) >= sizeof(m_contextState[0]) * MAX_OFF_CTX_MOD, "context state table is too small\n");
}

void Entropy::codeVPS(const VPS& vps)
{
    WRITE_CODE(0,       4, "vps_video_parameter_set_id");
    WRITE_CODE(3,       2, "vps_reserved_three_2bits");
    WRITE_CODE(0,       6, "vps_reserved_zero_6bits");
    WRITE_CODE(vps.maxTempSubLayers - 1, 3, "vps_max_sub_layers_minus1");
    WRITE_FLAG(vps.maxTempSubLayers == 1,   "vps_temporal_id_nesting_flag");
    WRITE_CODE(0xffff, 16, "vps_reserved_ffff_16bits");

    codeProfileTier(vps.ptl, vps.maxTempSubLayers);

    WRITE_FLAG(true, "vps_sub_layer_ordering_info_present_flag");

    for (uint32_t i = 0; i < vps.maxTempSubLayers; i++)
    {
        WRITE_UVLC(vps.maxDecPicBuffering - 1, "vps_max_dec_pic_buffering_minus1[i]");
        WRITE_UVLC(vps.numReorderPics,         "vps_num_reorder_pics[i]");
        WRITE_UVLC(vps.maxLatencyIncrease + 1, "vps_max_latency_increase_plus1[i]");
    }

    WRITE_CODE(0, 6, "vps_max_nuh_reserved_zero_layer_id");
    WRITE_UVLC(0,    "vps_max_op_sets_minus1");
    WRITE_FLAG(0,    "vps_timing_info_present_flag"); /* we signal timing info in SPS-VUI */
    WRITE_FLAG(0,    "vps_extension_flag");
}

void Entropy::codeSPS(const SPS& sps, const ScalingList& scalingList, const ProfileTierLevel& ptl)
{
    WRITE_CODE(0, 4, "sps_video_parameter_set_id");
    WRITE_CODE(sps.maxTempSubLayers - 1, 3, "sps_max_sub_layers_minus1");
    WRITE_FLAG(sps.maxTempSubLayers == 1,   "sps_temporal_id_nesting_flag");

    codeProfileTier(ptl, sps.maxTempSubLayers);

    WRITE_UVLC(0, "sps_seq_parameter_set_id");
    WRITE_UVLC(sps.chromaFormatIdc, "chroma_format_idc");

    if (sps.chromaFormatIdc == X265_CSP_I444)
        WRITE_FLAG(0,                       "separate_colour_plane_flag");

    WRITE_UVLC(sps.picWidthInLumaSamples,   "pic_width_in_luma_samples");
    WRITE_UVLC(sps.picHeightInLumaSamples,  "pic_height_in_luma_samples");

    const Window& conf = sps.conformanceWindow;
    WRITE_FLAG(conf.bEnabled, "conformance_window_flag");
    if (conf.bEnabled)
    {
        int hShift = CHROMA_H_SHIFT(sps.chromaFormatIdc), vShift = CHROMA_V_SHIFT(sps.chromaFormatIdc);
        WRITE_UVLC(conf.leftOffset   >> hShift, "conf_win_left_offset");
        WRITE_UVLC(conf.rightOffset  >> hShift, "conf_win_right_offset");
        WRITE_UVLC(conf.topOffset    >> vShift, "conf_win_top_offset");
        WRITE_UVLC(conf.bottomOffset >> vShift, "conf_win_bottom_offset");
    }

    WRITE_UVLC(X265_DEPTH - 8,   "bit_depth_luma_minus8");
    WRITE_UVLC(X265_DEPTH - 8,   "bit_depth_chroma_minus8");
    WRITE_UVLC(BITS_FOR_POC - 4, "log2_max_pic_order_cnt_lsb_minus4");
    WRITE_FLAG(true,             "sps_sub_layer_ordering_info_present_flag");

    for (uint32_t i = 0; i < sps.maxTempSubLayers; i++)
    {
        WRITE_UVLC(sps.maxDecPicBuffering - 1, "sps_max_dec_pic_buffering_minus1[i]");
        WRITE_UVLC(sps.numReorderPics,         "sps_num_reorder_pics[i]");
        WRITE_UVLC(sps.maxLatencyIncrease + 1, "sps_max_latency_increase_plus1[i]");
    }

    WRITE_UVLC(sps.log2MinCodingBlockSize - 3,    "log2_min_coding_block_size_minus3");
    WRITE_UVLC(sps.log2DiffMaxMinCodingBlockSize, "log2_diff_max_min_coding_block_size");
    WRITE_UVLC(sps.quadtreeTULog2MinSize - 2,     "log2_min_transform_block_size_minus2");
    WRITE_UVLC(sps.quadtreeTULog2MaxSize - sps.quadtreeTULog2MinSize, "log2_diff_max_min_transform_block_size");
    WRITE_UVLC(sps.quadtreeTUMaxDepthInter - 1,   "max_transform_hierarchy_depth_inter");
    WRITE_UVLC(sps.quadtreeTUMaxDepthIntra - 1,   "max_transform_hierarchy_depth_intra");
    WRITE_FLAG(scalingList.m_bEnabled,            "scaling_list_enabled_flag");
    if (scalingList.m_bEnabled)
    {
        WRITE_FLAG(scalingList.m_bDataPresent,    "sps_scaling_list_data_present_flag");
        if (scalingList.m_bDataPresent)
            codeScalingList(scalingList);
    }
    WRITE_FLAG(sps.bUseAMP, "amp_enabled_flag");
    WRITE_FLAG(sps.bUseSAO, "sample_adaptive_offset_enabled_flag");

    WRITE_FLAG(0, "pcm_enabled_flag");
    WRITE_UVLC(0, "num_short_term_ref_pic_sets");
    WRITE_FLAG(0, "long_term_ref_pics_present_flag");

    WRITE_FLAG(sps.bTemporalMVPEnabled, "sps_temporal_mvp_enable_flag");
    WRITE_FLAG(sps.bUseStrongIntraSmoothing, "sps_strong_intra_smoothing_enable_flag");

    WRITE_FLAG(1, "vui_parameters_present_flag");
    codeVUI(sps.vuiParameters, sps.maxTempSubLayers);

    WRITE_FLAG(0, "sps_extension_flag");
}

void Entropy::codePPS(const PPS& pps)
{
    WRITE_UVLC(0,                          "pps_pic_parameter_set_id");
    WRITE_UVLC(0,                          "pps_seq_parameter_set_id");
    WRITE_FLAG(0,                          "dependent_slice_segments_enabled_flag");
    WRITE_FLAG(0,                          "output_flag_present_flag");
    WRITE_CODE(0, 3,                       "num_extra_slice_header_bits");
    WRITE_FLAG(pps.bSignHideEnabled,       "sign_data_hiding_flag");
    WRITE_FLAG(0,                          "cabac_init_present_flag");
    WRITE_UVLC(0,                          "num_ref_idx_l0_default_active_minus1");
    WRITE_UVLC(0,                          "num_ref_idx_l1_default_active_minus1");

    WRITE_SVLC(0, "init_qp_minus26");
    WRITE_FLAG(pps.bConstrainedIntraPred, "constrained_intra_pred_flag");
    WRITE_FLAG(pps.bTransformSkipEnabled, "transform_skip_enabled_flag");

    WRITE_FLAG(pps.bUseDQP,                "cu_qp_delta_enabled_flag");
    if (pps.bUseDQP)
        WRITE_UVLC(pps.maxCuDQPDepth,      "diff_cu_qp_delta_depth");

    WRITE_SVLC(pps.chromaQpOffset[0],      "pps_cb_qp_offset");
    WRITE_SVLC(pps.chromaQpOffset[1],      "pps_cr_qp_offset");
    WRITE_FLAG(0,                          "pps_slice_chroma_qp_offsets_present_flag");

    WRITE_FLAG(pps.bUseWeightPred,            "weighted_pred_flag");
    WRITE_FLAG(pps.bUseWeightedBiPred,        "weighted_bipred_flag");
    WRITE_FLAG(pps.bTransquantBypassEnabled,  "transquant_bypass_enable_flag");
    WRITE_FLAG(0,                             "tiles_enabled_flag");
    WRITE_FLAG(pps.bEntropyCodingSyncEnabled, "entropy_coding_sync_enabled_flag");
    WRITE_FLAG(1,                             "loop_filter_across_slices_enabled_flag");

    WRITE_FLAG(pps.bDeblockingFilterControlPresent, "deblocking_filter_control_present_flag");
    if (pps.bDeblockingFilterControlPresent)
    {
        WRITE_FLAG(0,                               "deblocking_filter_override_enabled_flag");
        WRITE_FLAG(pps.bPicDisableDeblockingFilter, "pps_disable_deblocking_filter_flag");
        if (!pps.bPicDisableDeblockingFilter)
        {
            WRITE_SVLC(pps.deblockingFilterBetaOffsetDiv2, "pps_beta_offset_div2");
            WRITE_SVLC(pps.deblockingFilterTcOffsetDiv2,   "pps_tc_offset_div2");
        }
    }

    WRITE_FLAG(0, "pps_scaling_list_data_present_flag");
    WRITE_FLAG(0, "lists_modification_present_flag");
    WRITE_UVLC(0, "log2_parallel_merge_level_minus2");
    WRITE_FLAG(0, "slice_segment_header_extension_present_flag");
    WRITE_FLAG(0, "pps_extension_flag");
}

void Entropy::codeProfileTier(const ProfileTierLevel& ptl, int maxTempSubLayers)
{
    WRITE_CODE(0, 2,                "XXX_profile_space[]");
    WRITE_FLAG(ptl.tierFlag,        "XXX_tier_flag[]");
    WRITE_CODE(ptl.profileIdc, 5,   "XXX_profile_idc[]");
    for (int j = 0; j < 32; j++)
        WRITE_FLAG(ptl.profileCompatibilityFlag[j], "XXX_profile_compatibility_flag[][j]");

    WRITE_FLAG(ptl.progressiveSourceFlag,   "general_progressive_source_flag");
    WRITE_FLAG(ptl.interlacedSourceFlag,    "general_interlaced_source_flag");
    WRITE_FLAG(ptl.nonPackedConstraintFlag, "general_non_packed_constraint_flag");
    WRITE_FLAG(ptl.frameOnlyConstraintFlag, "general_frame_only_constraint_flag");

    if (ptl.profileIdc == Profile::MAINREXT || ptl.profileIdc == Profile::HIGHTHROUGHPUTREXT)
    {
        uint32_t bitDepthConstraint = ptl.bitDepthConstraint;
        int csp = ptl.chromaFormatConstraint;
        WRITE_FLAG(bitDepthConstraint<=12, "general_max_12bit_constraint_flag");
        WRITE_FLAG(bitDepthConstraint<=10, "general_max_10bit_constraint_flag");
        WRITE_FLAG(bitDepthConstraint<= 8 && csp != X265_CSP_I422 , "general_max_8bit_constraint_flag");
        WRITE_FLAG(csp == X265_CSP_I422 || csp == X265_CSP_I420 || csp == X265_CSP_I400, "general_max_422chroma_constraint_flag");
        WRITE_FLAG(csp == X265_CSP_I420 || csp == X265_CSP_I400,                         "general_max_420chroma_constraint_flag");
        WRITE_FLAG(csp == X265_CSP_I400,                                                 "general_max_monochrome_constraint_flag");
        WRITE_FLAG(ptl.intraConstraintFlag,        "general_intra_constraint_flag");
        WRITE_FLAG(0,                              "general_one_picture_only_constraint_flag");
        WRITE_FLAG(ptl.lowerBitRateConstraintFlag, "general_lower_bit_rate_constraint_flag");
        WRITE_CODE(0 , 16, "XXX_reserved_zero_35bits[0..15]");
        WRITE_CODE(0 , 16, "XXX_reserved_zero_35bits[16..31]");
        WRITE_CODE(0 ,  3, "XXX_reserved_zero_35bits[32..34]");
    }
    else
    {
        WRITE_CODE(0, 16, "XXX_reserved_zero_44bits[0..15]");
        WRITE_CODE(0, 16, "XXX_reserved_zero_44bits[16..31]");
        WRITE_CODE(0, 12, "XXX_reserved_zero_44bits[32..43]");
    }

    WRITE_CODE(ptl.levelIdc, 8, "general_level_idc");

    if (maxTempSubLayers > 1)
    {
         WRITE_FLAG(0, "sub_layer_profile_present_flag[i]");
         WRITE_FLAG(0, "sub_layer_level_present_flag[i]");
         for (int i = maxTempSubLayers - 1; i < 8 ; i++)
             WRITE_CODE(0, 2, "reserved_zero_2bits");
    }
}

void Entropy::codeVUI(const VUI& vui, int maxSubTLayers)
{
    WRITE_FLAG(vui.aspectRatioInfoPresentFlag,  "aspect_ratio_info_present_flag");
    if (vui.aspectRatioInfoPresentFlag)
    {
        WRITE_CODE(vui.aspectRatioIdc, 8,       "aspect_ratio_idc");
        if (vui.aspectRatioIdc == 255)
        {
            WRITE_CODE(vui.sarWidth, 16,        "sar_width");
            WRITE_CODE(vui.sarHeight, 16,       "sar_height");
        }
    }

    WRITE_FLAG(vui.overscanInfoPresentFlag,     "overscan_info_present_flag");
    if (vui.overscanInfoPresentFlag)
        WRITE_FLAG(vui.overscanAppropriateFlag, "overscan_appropriate_flag");

    WRITE_FLAG(vui.videoSignalTypePresentFlag,  "video_signal_type_present_flag");
    if (vui.videoSignalTypePresentFlag)
    {
        WRITE_CODE(vui.videoFormat, 3,          "video_format");
        WRITE_FLAG(vui.videoFullRangeFlag,      "video_full_range_flag");
        WRITE_FLAG(vui.colourDescriptionPresentFlag, "colour_description_present_flag");
        if (vui.colourDescriptionPresentFlag)
        {
            WRITE_CODE(vui.colourPrimaries, 8,         "colour_primaries");
            WRITE_CODE(vui.transferCharacteristics, 8, "transfer_characteristics");
            WRITE_CODE(vui.matrixCoefficients, 8,      "matrix_coefficients");
        }
    }

    WRITE_FLAG(vui.chromaLocInfoPresentFlag,           "chroma_loc_info_present_flag");
    if (vui.chromaLocInfoPresentFlag)
    {
        WRITE_UVLC(vui.chromaSampleLocTypeTopField,    "chroma_sample_loc_type_top_field");
        WRITE_UVLC(vui.chromaSampleLocTypeBottomField, "chroma_sample_loc_type_bottom_field");
    }

    WRITE_FLAG(0,                                     "neutral_chroma_indication_flag");
    WRITE_FLAG(vui.fieldSeqFlag,                      "field_seq_flag");
    WRITE_FLAG(vui.frameFieldInfoPresentFlag,         "frame_field_info_present_flag");

    WRITE_FLAG(vui.defaultDisplayWindow.bEnabled,    "default_display_window_flag");
    if (vui.defaultDisplayWindow.bEnabled)
    {
        WRITE_UVLC(vui.defaultDisplayWindow.leftOffset,   "def_disp_win_left_offset");
        WRITE_UVLC(vui.defaultDisplayWindow.rightOffset,  "def_disp_win_right_offset");
        WRITE_UVLC(vui.defaultDisplayWindow.topOffset,    "def_disp_win_top_offset");
        WRITE_UVLC(vui.defaultDisplayWindow.bottomOffset, "def_disp_win_bottom_offset");
    }

    WRITE_FLAG(1,                                 "vui_timing_info_present_flag");
    WRITE_CODE(vui.timingInfo.numUnitsInTick, 32, "vui_num_units_in_tick");
    WRITE_CODE(vui.timingInfo.timeScale, 32,      "vui_time_scale");
    WRITE_FLAG(0,                                 "vui_poc_proportional_to_timing_flag");

    WRITE_FLAG(vui.hrdParametersPresentFlag,  "vui_hrd_parameters_present_flag");
    if (vui.hrdParametersPresentFlag)
        codeHrdParameters(vui.hrdParameters, maxSubTLayers);

    WRITE_FLAG(0, "bitstream_restriction_flag");
}

void Entropy::codeScalingList(const ScalingList& scalingList)
{
    for (int sizeId = 0; sizeId < ScalingList::NUM_SIZES; sizeId++)
    {
        for (int listId = 0; listId < ScalingList::NUM_LISTS; listId++)
        {
            int predList = scalingList.checkPredMode(sizeId, listId);
            WRITE_FLAG(predList < 0, "scaling_list_pred_mode_flag");
            if (predList >= 0)
                WRITE_UVLC(listId - predList, "scaling_list_pred_matrix_id_delta");
            else // DPCM Mode
                codeScalingList(scalingList, sizeId, listId);
        }
    }
}

void Entropy::codeScalingList(const ScalingList& scalingList, uint32_t sizeId, uint32_t listId)
{
    int coefNum = X265_MIN(ScalingList::MAX_MATRIX_COEF_NUM, (int)ScalingList::s_numCoefPerSize[sizeId]);
    const uint16_t* scan = (sizeId == 0 ? g_scan4x4[SCAN_DIAG] : g_scan8x8diag);
    int nextCoef = START_VALUE;
    int32_t *src = scalingList.m_scalingListCoef[sizeId][listId];
    int data;

    if (sizeId > BLOCK_8x8)
    {
        WRITE_SVLC(scalingList.m_scalingListDC[sizeId][listId] - 8, "scaling_list_dc_coef_minus8");
        nextCoef = scalingList.m_scalingListDC[sizeId][listId];
    }
    for (int i = 0; i < coefNum; i++)
    {
        data = src[scan[i]] - nextCoef;
        nextCoef = src[scan[i]];
        if (data > 127)
            data = data - 256;
        if (data < -128)
            data = data + 256;

        WRITE_SVLC(data,  "scaling_list_delta_coef");
    }
}

void Entropy::codeHrdParameters(const HRDInfo& hrd, int maxSubTLayers)
{
    WRITE_FLAG(1, "nal_hrd_parameters_present_flag");
    WRITE_FLAG(0, "vcl_hrd_parameters_present_flag");
    WRITE_FLAG(0, "sub_pic_hrd_params_present_flag");

    WRITE_CODE(hrd.bitRateScale, 4, "bit_rate_scale");
    WRITE_CODE(hrd.cpbSizeScale, 4, "cpb_size_scale");

    WRITE_CODE(hrd.initialCpbRemovalDelayLength - 1, 5, "initial_cpb_removal_delay_length_minus1");
    WRITE_CODE(hrd.cpbRemovalDelayLength - 1,        5, "au_cpb_removal_delay_length_minus1");
    WRITE_CODE(hrd.dpbOutputDelayLength - 1,         5, "dpb_output_delay_length_minus1");

    for (int i = 0; i < maxSubTLayers; i++)
    {
        WRITE_FLAG(1, "fixed_pic_rate_general_flag");
        WRITE_UVLC(0, "elemental_duration_in_tc_minus1");
        WRITE_UVLC(0, "cpb_cnt_minus1");

        WRITE_UVLC(hrd.bitRateValue - 1, "bit_rate_value_minus1");
        WRITE_UVLC(hrd.cpbSizeValue - 1, "cpb_size_value_minus1");
        WRITE_FLAG(hrd.cbrFlag, "cbr_flag");
    }
}

void Entropy::codeAUD(const Slice& slice)
{
    int picType;

    switch (slice.m_sliceType)
    {
    case I_SLICE:
        picType = 0;
        break;
    case P_SLICE:
        picType = 1;
        break;
    case B_SLICE:
        picType = 2;
        break;
    default:
        picType = 7;
        break;
    }

    WRITE_CODE(picType, 3, "pic_type");
}

void Entropy::codeSliceHeader(const Slice& slice, FrameData& encData)
{
    WRITE_FLAG(1, "first_slice_segment_in_pic_flag");
    if (slice.getRapPicFlag())
        WRITE_FLAG(0, "no_output_of_prior_pics_flag");

    WRITE_UVLC(0, "slice_pic_parameter_set_id");

    /* x265 does not use dependent slices, so always write all this data */

    WRITE_UVLC(slice.m_sliceType, "slice_type");

    if (!slice.getIdrPicFlag())
    {
        int picOrderCntLSB = (slice.m_poc - slice.m_lastIDR + (1 << BITS_FOR_POC)) % (1 << BITS_FOR_POC);
        WRITE_CODE(picOrderCntLSB, BITS_FOR_POC, "pic_order_cnt_lsb");

#if _DEBUG || CHECKED_BUILD
        // check for bitstream restriction stating that:
        // If the current picture is a BLA or CRA picture, the value of NumPocTotalCurr shall be equal to 0.
        // Ideally this process should not be repeated for each slice in a picture
        if (slice.isIRAP())
            for (int picIdx = 0; picIdx < slice.m_rps.numberOfPictures; picIdx++)
            {
                X265_CHECK(!slice.m_rps.bUsed[picIdx], "pic unused failure\n");
            }
#endif

        WRITE_FLAG(0, "short_term_ref_pic_set_sps_flag");
        codeShortTermRefPicSet(slice.m_rps);

        if (slice.m_sps->bTemporalMVPEnabled)
            WRITE_FLAG(1, "slice_temporal_mvp_enable_flag");
    }
    const SAOParam *saoParam = encData.m_saoParam;
    if (slice.m_sps->bUseSAO)
    {
        WRITE_FLAG(saoParam->bSaoFlag[0], "slice_sao_luma_flag");
        WRITE_FLAG(saoParam->bSaoFlag[1], "slice_sao_chroma_flag");
    }

    // check if numRefIdx match the defaults (1, hard-coded in PPS). If not, override
    // TODO: this might be a place to optimize a few bits per slice, by using param->refs for L0 default

    if (!slice.isIntra())
    {
        bool overrideFlag = (slice.m_numRefIdx[0] != 1 || (slice.isInterB() && slice.m_numRefIdx[1] != 1));
        WRITE_FLAG(overrideFlag, "num_ref_idx_active_override_flag");
        if (overrideFlag)
        {
            WRITE_UVLC(slice.m_numRefIdx[0] - 1, "num_ref_idx_l0_active_minus1");
            if (slice.isInterB())
                WRITE_UVLC(slice.m_numRefIdx[1] - 1, "num_ref_idx_l1_active_minus1");
            else
            {
                X265_CHECK(slice.m_numRefIdx[1] == 0, "expected no L1 references for P slice\n");
            }
        }
    }
    else
    {
        X265_CHECK(!slice.m_numRefIdx[0] && !slice.m_numRefIdx[1], "expected no references for I slice\n");
    }

    if (slice.isInterB())
        WRITE_FLAG(0, "mvd_l1_zero_flag");

    if (slice.m_sps->bTemporalMVPEnabled)
    {
        if (slice.m_sliceType == B_SLICE)
            WRITE_FLAG(slice.m_colFromL0Flag, "collocated_from_l0_flag");

        if (slice.m_sliceType != I_SLICE &&
            ((slice.m_colFromL0Flag && slice.m_numRefIdx[0] > 1) ||
            (!slice.m_colFromL0Flag && slice.m_numRefIdx[1] > 1)))
        {
            WRITE_UVLC(slice.m_colRefIdx, "collocated_ref_idx");
        }
    }
    if ((slice.m_pps->bUseWeightPred && slice.m_sliceType == P_SLICE) || (slice.m_pps->bUseWeightedBiPred && slice.m_sliceType == B_SLICE))
        codePredWeightTable(slice);

    X265_CHECK(slice.m_maxNumMergeCand <= MRG_MAX_NUM_CANDS, "too many merge candidates\n");
    if (!slice.isIntra())
        WRITE_UVLC(MRG_MAX_NUM_CANDS - slice.m_maxNumMergeCand, "five_minus_max_num_merge_cand");

    int code = slice.m_sliceQp - 26;
    WRITE_SVLC(code, "slice_qp_delta");

    bool isSAOEnabled = slice.m_sps->bUseSAO ? saoParam->bSaoFlag[0] || saoParam->bSaoFlag[1] : false;
    bool isDBFEnabled = !slice.m_pps->bPicDisableDeblockingFilter;

    if (isSAOEnabled || isDBFEnabled)
        WRITE_FLAG(slice.m_sLFaseFlag, "slice_loop_filter_across_slices_enabled_flag");
}

/** write wavefront substreams sizes for the slice header */
void Entropy::codeSliceHeaderWPPEntryPoints(const Slice& slice, const uint32_t *substreamSizes, uint32_t maxOffset)
{
    uint32_t offsetLen = 1;
    while (maxOffset >= (1U << offsetLen))
    {
        offsetLen++;
        X265_CHECK(offsetLen < 32, "offsetLen is too large\n");
    }

    uint32_t numRows = slice.m_sps->numCuInHeight - 1;
    WRITE_UVLC(numRows, "num_entry_point_offsets");
    if (numRows > 0)
        WRITE_UVLC(offsetLen - 1, "offset_len_minus1");

    for (uint32_t i = 0; i < numRows; i++)
        WRITE_CODE(substreamSizes[i] - 1, offsetLen, "entry_point_offset_minus1");
}

void Entropy::codeShortTermRefPicSet(const RPS& rps)
{
    WRITE_UVLC(rps.numberOfNegativePictures, "num_negative_pics");
    WRITE_UVLC(rps.numberOfPositivePictures, "num_positive_pics");
    int prev = 0;
    for (int j = 0; j < rps.numberOfNegativePictures; j++)
    {
        WRITE_UVLC(prev - rps.deltaPOC[j] - 1, "delta_poc_s0_minus1");
        prev = rps.deltaPOC[j];
        WRITE_FLAG(rps.bUsed[j], "used_by_curr_pic_s0_flag");
    }

    prev = 0;
    for (int j = rps.numberOfNegativePictures; j < rps.numberOfNegativePictures + rps.numberOfPositivePictures; j++)
    {
        WRITE_UVLC(rps.deltaPOC[j] - prev - 1, "delta_poc_s1_minus1");
        prev = rps.deltaPOC[j];
        WRITE_FLAG(rps.bUsed[j], "used_by_curr_pic_s1_flag");
    }
}

void Entropy::encodeCTU(const CUData& ctu, const CUGeom& cuGeom)
{
    bool bEncodeDQP = ctu.m_slice->m_pps->bUseDQP;
    encodeCU(ctu, cuGeom, 0, 0, bEncodeDQP);
}

/* encode a CU block recursively */
void Entropy::encodeCU(const CUData& ctu, const CUGeom& cuGeom, uint32_t absPartIdx, uint32_t depth, bool& bEncodeDQP)
{
    const Slice* slice = ctu.m_slice;

    int cuSplitFlag = !(cuGeom.flags & CUGeom::LEAF);
    int cuUnsplitFlag = !(cuGeom.flags & CUGeom::SPLIT_MANDATORY);

    if (!cuUnsplitFlag)
    {
        uint32_t qNumParts = cuGeom.numPartitions >> 2;
        if (depth == slice->m_pps->maxCuDQPDepth && slice->m_pps->bUseDQP)
            bEncodeDQP = true;
        for (uint32_t qIdx = 0; qIdx < 4; ++qIdx, absPartIdx += qNumParts)
        {
            const CUGeom& childGeom = *(&cuGeom + cuGeom.childOffset + qIdx);
            if (childGeom.flags & CUGeom::PRESENT)
                encodeCU(ctu, childGeom, absPartIdx, depth + 1, bEncodeDQP);
        }
        return;
    }

    if (cuSplitFlag) 
        codeSplitFlag(ctu, absPartIdx, depth);

    if (depth < ctu.m_cuDepth[absPartIdx] && depth < g_maxCUDepth)
    {
        uint32_t qNumParts = cuGeom.numPartitions >> 2;
        if (depth == slice->m_pps->maxCuDQPDepth && slice->m_pps->bUseDQP)
            bEncodeDQP = true;
        for (uint32_t qIdx = 0; qIdx < 4; ++qIdx, absPartIdx += qNumParts)
        {
            const CUGeom& childGeom = *(&cuGeom + cuGeom.childOffset + qIdx);
            encodeCU(ctu, childGeom, absPartIdx, depth + 1, bEncodeDQP);
        }
        return;
    }

    if (depth <= slice->m_pps->maxCuDQPDepth && slice->m_pps->bUseDQP)
        bEncodeDQP = true;

    if (slice->m_pps->bTransquantBypassEnabled)
        codeCUTransquantBypassFlag(ctu.m_tqBypass[absPartIdx]);

    if (!slice->isIntra())
    {
        codeSkipFlag(ctu, absPartIdx);
        if (ctu.isSkipped(absPartIdx))
        {
            codeMergeIndex(ctu, absPartIdx);
            finishCU(ctu, absPartIdx, depth, bEncodeDQP);
            return;
        }
        codePredMode(ctu.m_predMode[absPartIdx]);
    }

    codePartSize(ctu, absPartIdx, depth);

    // prediction Info ( Intra : direction mode, Inter : Mv, reference idx )
    codePredInfo(ctu, absPartIdx);

    uint32_t tuDepthRange[2];
    if (ctu.isIntra(absPartIdx))
        ctu.getIntraTUQtDepthRange(tuDepthRange, absPartIdx);
    else
        ctu.getInterTUQtDepthRange(tuDepthRange, absPartIdx);

    // Encode Coefficients, allow codeCoeff() to modify bEncodeDQP
    codeCoeff(ctu, absPartIdx, bEncodeDQP, tuDepthRange);

    // --- write terminating bit ---
    finishCU(ctu, absPartIdx, depth, bEncodeDQP);
}

/* Return bit count of signaling inter mode */
uint32_t Entropy::bitsInterMode(const CUData& cu, uint32_t absPartIdx, uint32_t depth) const
{
    uint32_t bits;
    bits = bitsCodeBin(0, m_contextState[OFF_SKIP_FLAG_CTX + cu.getCtxSkipFlag(absPartIdx)]); /* not skip */
    bits += bitsCodeBin(0, m_contextState[OFF_PRED_MODE_CTX]); /* inter */
    PartSize partSize = (PartSize)cu.m_partSize[absPartIdx];
    switch (partSize)
    {
    case SIZE_2Nx2N:
        bits += bitsCodeBin(1, m_contextState[OFF_PART_SIZE_CTX]);
        break;

    case SIZE_2NxN:
    case SIZE_2NxnU:
    case SIZE_2NxnD:
        bits += bitsCodeBin(0, m_contextState[OFF_PART_SIZE_CTX + 0]);
        bits += bitsCodeBin(1, m_contextState[OFF_PART_SIZE_CTX + 1]);
        if (cu.m_slice->m_sps->maxAMPDepth > depth)
        {
            bits += bitsCodeBin((partSize == SIZE_2NxN) ? 1 : 0, m_contextState[OFF_PART_SIZE_CTX + 3]);
            if (partSize != SIZE_2NxN)
                bits++; // encodeBinEP((partSize == SIZE_2NxnU ? 0 : 1));
        }
        break;

    case SIZE_Nx2N:
    case SIZE_nLx2N:
    case SIZE_nRx2N:
        bits += bitsCodeBin(0, m_contextState[OFF_PART_SIZE_CTX + 0]);
        bits += bitsCodeBin(0, m_contextState[OFF_PART_SIZE_CTX + 1]);
        if (depth == g_maxCUDepth && !(cu.m_log2CUSize[absPartIdx] == 3))
            bits += bitsCodeBin(1, m_contextState[OFF_PART_SIZE_CTX + 2]);
        if (cu.m_slice->m_sps->maxAMPDepth > depth)
        {
            bits += bitsCodeBin((partSize == SIZE_Nx2N) ? 1 : 0, m_contextState[OFF_PART_SIZE_CTX + 3]);
            if (partSize != SIZE_Nx2N)
                bits++; // encodeBinEP((partSize == SIZE_nLx2N ? 0 : 1));
        }
        break;
    default:
        X265_CHECK(0, "invalid CU partition\n");
        break;
    }

    return bits;
}

/* finish encoding a cu and handle end-of-slice conditions */
void Entropy::finishCU(const CUData& ctu, uint32_t absPartIdx, uint32_t depth, bool bCodeDQP)
{
    const Slice* slice = ctu.m_slice;
    uint32_t realEndAddress = slice->m_endCUAddr;
    uint32_t cuAddr = ctu.getSCUAddr() + absPartIdx;
    X265_CHECK(realEndAddress == slice->realEndAddress(slice->m_endCUAddr), "real end address expected\n");

    uint32_t granularityMask = g_maxCUSize - 1;
    uint32_t cuSize = 1 << ctu.m_log2CUSize[absPartIdx];
    uint32_t rpelx = ctu.m_cuPelX + g_zscanToPelX[absPartIdx] + cuSize;
    uint32_t bpely = ctu.m_cuPelY + g_zscanToPelY[absPartIdx] + cuSize;
    bool granularityBoundary = (((rpelx & granularityMask) == 0 || (rpelx == slice->m_sps->picWidthInLumaSamples )) &&
                                ((bpely & granularityMask) == 0 || (bpely == slice->m_sps->picHeightInLumaSamples)));

    if (slice->m_pps->bUseDQP)
        const_cast<CUData&>(ctu).setQPSubParts(bCodeDQP ? ctu.getRefQP(absPartIdx) : ctu.m_qp[absPartIdx], absPartIdx, depth);

    if (granularityBoundary)
    {
        // Encode slice finish
        bool bTerminateSlice = false;
        if (cuAddr + (NUM_4x4_PARTITIONS >> (depth << 1)) == realEndAddress)
            bTerminateSlice = true;

        // The 1-terminating bit is added to all streams, so don't add it here when it's 1.
        if (!bTerminateSlice)
            encodeBinTrm(0);

        if (!m_bitIf)
            resetBits(); // TODO: most likely unnecessary
    }
}

void Entropy::encodeTransform(const CUData& cu, uint32_t absPartIdx, uint32_t curDepth, uint32_t log2CurSize,
                              bool& bCodeDQP, const uint32_t depthRange[2])
{
    const bool subdiv = cu.m_tuDepth[absPartIdx] > curDepth;

    /* in each of these conditions, the subdiv flag is implied and not signaled,
     * so we have checks to make sure the implied value matches our intentions */
    if (cu.isIntra(absPartIdx) && cu.m_partSize[absPartIdx] != SIZE_2Nx2N && log2CurSize == MIN_LOG2_CU_SIZE)
    {
        X265_CHECK(subdiv, "intra NxN requires TU depth below CU depth\n");
    }
    else if (cu.isInter(absPartIdx) && cu.m_partSize[absPartIdx] != SIZE_2Nx2N &&
             !curDepth && cu.m_slice->m_sps->quadtreeTUMaxDepthInter == 1)
    {
        X265_CHECK(subdiv, "inter TU must be smaller than CU when not 2Nx2N part size: log2CurSize %d, depthRange[0] %d\n", log2CurSize, depthRange[0]);
    }
    else if (log2CurSize > depthRange[1])
    {
        X265_CHECK(subdiv, "TU is larger than the max allowed, it should have been split\n");
    }
    else if (log2CurSize == cu.m_slice->m_sps->quadtreeTULog2MinSize || log2CurSize == depthRange[0])
    {
        X265_CHECK(!subdiv, "min sized TU cannot be subdivided\n");
    }
    else
    {
        X265_CHECK(log2CurSize > depthRange[0], "transform size failure\n");
        codeTransformSubdivFlag(subdiv, 5 - log2CurSize);
    }

    uint32_t hChromaShift = cu.m_hChromaShift;
    uint32_t vChromaShift = cu.m_vChromaShift;
    bool bSmallChroma = (log2CurSize - hChromaShift) < 2;
    if (!curDepth || !bSmallChroma)
    {
        if (!curDepth || cu.getCbf(absPartIdx, TEXT_CHROMA_U, curDepth - 1))
            codeQtCbfChroma(cu, absPartIdx, TEXT_CHROMA_U, curDepth, !subdiv);
        if (!curDepth || cu.getCbf(absPartIdx, TEXT_CHROMA_V, curDepth - 1))
            codeQtCbfChroma(cu, absPartIdx, TEXT_CHROMA_V, curDepth, !subdiv);
    }
    else
    {
        X265_CHECK(cu.getCbf(absPartIdx, TEXT_CHROMA_U, curDepth) == cu.getCbf(absPartIdx, TEXT_CHROMA_U, curDepth - 1), "chroma xform size match failure\n");
        X265_CHECK(cu.getCbf(absPartIdx, TEXT_CHROMA_V, curDepth) == cu.getCbf(absPartIdx, TEXT_CHROMA_V, curDepth - 1), "chroma xform size match failure\n");
    }

    if (subdiv)
    {
        --log2CurSize;
        ++curDepth;

        uint32_t qNumParts = 1 << (log2CurSize - LOG2_UNIT_SIZE) * 2;

        encodeTransform(cu, absPartIdx + 0 * qNumParts, curDepth, log2CurSize, bCodeDQP, depthRange);
        encodeTransform(cu, absPartIdx + 1 * qNumParts, curDepth, log2CurSize, bCodeDQP, depthRange);
        encodeTransform(cu, absPartIdx + 2 * qNumParts, curDepth, log2CurSize, bCodeDQP, depthRange);
        encodeTransform(cu, absPartIdx + 3 * qNumParts, curDepth, log2CurSize, bCodeDQP, depthRange);
        return;
    }

    uint32_t absPartIdxC = bSmallChroma ? absPartIdx & 0xFC : absPartIdx;

    if (cu.isInter(absPartIdxC) && !curDepth && !cu.getCbf(absPartIdxC, TEXT_CHROMA_U, 0) && !cu.getCbf(absPartIdxC, TEXT_CHROMA_V, 0))
    {
        X265_CHECK(cu.getCbf(absPartIdxC, TEXT_LUMA, 0), "CBF should have been set\n");
    }
    else
        codeQtCbfLuma(cu, absPartIdx, curDepth);

    uint32_t cbfY = cu.getCbf(absPartIdx, TEXT_LUMA, curDepth);
    uint32_t cbfU = cu.getCbf(absPartIdxC, TEXT_CHROMA_U, curDepth);
    uint32_t cbfV = cu.getCbf(absPartIdxC, TEXT_CHROMA_V, curDepth);
    if (!(cbfY || cbfU || cbfV))
        return;

    // dQP: only for CTU once
    if (cu.m_slice->m_pps->bUseDQP && bCodeDQP)
    {
        uint32_t log2CUSize = cu.m_log2CUSize[absPartIdx];
        uint32_t absPartIdxLT = absPartIdx & (0xFF << (log2CUSize - LOG2_UNIT_SIZE) * 2);
        codeDeltaQP(cu, absPartIdxLT);
        bCodeDQP = false;
    }

    if (cbfY)
    {
        uint32_t coeffOffset = absPartIdx << (LOG2_UNIT_SIZE * 2);
        codeCoeffNxN(cu, cu.m_trCoeff[0] + coeffOffset, absPartIdx, log2CurSize, TEXT_LUMA);
        if (!(cbfU || cbfV))
            return;
    }

    if (bSmallChroma)
    {
        if ((absPartIdx & 3) != 3)
            return;

        const uint32_t log2CurSizeC = 2;
        const bool splitIntoSubTUs = (cu.m_chromaFormat == X265_CSP_I422);
        const uint32_t curPartNum = 4;
        uint32_t coeffOffsetC  = absPartIdxC << (LOG2_UNIT_SIZE * 2 - (hChromaShift + vChromaShift));
        for (uint32_t chromaId = TEXT_CHROMA_U; chromaId <= TEXT_CHROMA_V; chromaId++)
        {
            TURecurse tuIterator(splitIntoSubTUs ? VERTICAL_SPLIT : DONT_SPLIT, curPartNum, absPartIdxC);
            const coeff_t* coeffChroma = cu.m_trCoeff[chromaId];
            do
            {
                if (cu.getCbf(tuIterator.absPartIdxTURelCU, (TextType)chromaId, curDepth + splitIntoSubTUs))
                {
                    uint32_t subTUOffset = tuIterator.section << (log2CurSizeC * 2);
                    codeCoeffNxN(cu, coeffChroma + coeffOffsetC + subTUOffset, tuIterator.absPartIdxTURelCU, log2CurSizeC, (TextType)chromaId);
                }
            }
            while (tuIterator.isNextSection());
        }
    }
    else
    {
        uint32_t log2CurSizeC = log2CurSize - hChromaShift;
        const bool splitIntoSubTUs = (cu.m_chromaFormat == X265_CSP_I422);
        uint32_t curPartNum = 1 << (log2CurSize - LOG2_UNIT_SIZE) * 2;
        uint32_t coeffOffsetC  = absPartIdxC << (LOG2_UNIT_SIZE * 2 - (hChromaShift + vChromaShift));
        for (uint32_t chromaId = TEXT_CHROMA_U; chromaId <= TEXT_CHROMA_V; chromaId++)
        {
            TURecurse tuIterator(splitIntoSubTUs ? VERTICAL_SPLIT : DONT_SPLIT, curPartNum, absPartIdxC);
            const coeff_t* coeffChroma = cu.m_trCoeff[chromaId];
            do
            {
                if (cu.getCbf(tuIterator.absPartIdxTURelCU, (TextType)chromaId, curDepth + splitIntoSubTUs))
                {
                    uint32_t subTUOffset = tuIterator.section << (log2CurSizeC * 2);
                    codeCoeffNxN(cu, coeffChroma + coeffOffsetC + subTUOffset, tuIterator.absPartIdxTURelCU, log2CurSizeC, (TextType)chromaId);
                }
            }
            while (tuIterator.isNextSection());
        }
    }
}

void Entropy::codePredInfo(const CUData& cu, uint32_t absPartIdx)
{
    if (cu.isIntra(absPartIdx)) // If it is intra mode, encode intra prediction mode.
    {
        codeIntraDirLumaAng(cu, absPartIdx, true);
        if (cu.m_chromaFormat != X265_CSP_I400)
        {
            uint32_t chromaDirMode[NUM_CHROMA_MODE];
            cu.getAllowedChromaDir(absPartIdx, chromaDirMode);

            codeIntraDirChroma(cu, absPartIdx, chromaDirMode);

            if (cu.m_chromaFormat == X265_CSP_I444 && cu.m_partSize[absPartIdx] != SIZE_2Nx2N)
            {
                uint32_t qNumParts = 1 << (cu.m_log2CUSize[absPartIdx] - 1 - LOG2_UNIT_SIZE) * 2;
                for (uint32_t qIdx = 1; qIdx < 4; ++qIdx)
                {
                    absPartIdx += qNumParts;
                    cu.getAllowedChromaDir(absPartIdx, chromaDirMode);
                    codeIntraDirChroma(cu, absPartIdx, chromaDirMode);
                }
            }
        }
    }
    else // if it is inter mode, encode motion vector and reference index
        codePUWise(cu, absPartIdx);
}

/** encode motion information for every PU block */
void Entropy::codePUWise(const CUData& cu, uint32_t absPartIdx)
{
    X265_CHECK(!cu.isIntra(absPartIdx), "intra block not expected\n");
    PartSize partSize = (PartSize)cu.m_partSize[absPartIdx];
    uint32_t numPU = (partSize == SIZE_2Nx2N ? 1 : (partSize == SIZE_NxN ? 4 : 2));
    uint32_t depth = cu.m_cuDepth[absPartIdx];
    uint32_t puOffset = (g_puOffset[uint32_t(partSize)] << (g_unitSizeDepth - depth) * 2) >> 4;

    for (uint32_t puIdx = 0, subPartIdx = absPartIdx; puIdx < numPU; puIdx++, subPartIdx += puOffset)
    {
        codeMergeFlag(cu, subPartIdx);
        if (cu.m_mergeFlag[subPartIdx])
            codeMergeIndex(cu, subPartIdx);
        else
        {
            if (cu.m_slice->isInterB())
                codeInterDir(cu, subPartIdx);

            uint32_t interDir = cu.m_interDir[subPartIdx];
            for (uint32_t list = 0; list < 2; list++)
            {
                if (interDir & (1 << list))
                {
                    X265_CHECK(cu.m_slice->m_numRefIdx[list] > 0, "numRefs should have been > 0\n");

                    codeRefFrmIdxPU(cu, subPartIdx, list);
                    codeMvd(cu, subPartIdx, list);
                    codeMVPIdx(cu.m_mvpIdx[list][subPartIdx]);
                }
            }
        }
    }
}

/** encode reference frame index for a PU block */
void Entropy::codeRefFrmIdxPU(const CUData& cu, uint32_t absPartIdx, int list)
{
    X265_CHECK(!cu.isIntra(absPartIdx), "intra block not expected\n");

    if (cu.m_slice->m_numRefIdx[list] > 1)
        codeRefFrmIdx(cu, absPartIdx, list);
}

void Entropy::codeCoeff(const CUData& cu, uint32_t absPartIdx, bool& bCodeDQP, const uint32_t depthRange[2])
{
    if (!cu.isIntra(absPartIdx))
    {
        if (!(cu.m_mergeFlag[absPartIdx] && cu.m_partSize[absPartIdx] == SIZE_2Nx2N))
            codeQtRootCbf(cu.getQtRootCbf(absPartIdx));
        if (!cu.getQtRootCbf(absPartIdx))
            return;
    }

    uint32_t log2CUSize = cu.m_log2CUSize[absPartIdx];
    encodeTransform(cu, absPartIdx, 0, log2CUSize, bCodeDQP, depthRange);
}

void Entropy::codeSaoOffset(const SaoCtuParam& ctuParam, int plane)
{
    int typeIdx = ctuParam.typeIdx;

    if (plane != 2)
    {
        encodeBin(typeIdx >= 0, m_contextState[OFF_SAO_TYPE_IDX_CTX]);
        if (typeIdx >= 0)
            encodeBinEP(typeIdx < SAO_BO ? 1 : 0);
    }

    if (typeIdx >= 0)
    {
        enum { OFFSET_THRESH = 1 << X265_MIN(X265_DEPTH - 5, 5) };
        if (typeIdx == SAO_BO)
        {
            for (int i = 0; i < SAO_BO_LEN; i++)
                codeSaoMaxUvlc(abs(ctuParam.offset[i]), OFFSET_THRESH - 1);

            for (int i = 0; i < SAO_BO_LEN; i++)
                if (ctuParam.offset[i] != 0)
                    encodeBinEP(ctuParam.offset[i] < 0);

            encodeBinsEP(ctuParam.bandPos, 5);
        }
        else // if (typeIdx < SAO_BO)
        {
            codeSaoMaxUvlc(ctuParam.offset[0], OFFSET_THRESH - 1);
            codeSaoMaxUvlc(ctuParam.offset[1], OFFSET_THRESH - 1);
            codeSaoMaxUvlc(-ctuParam.offset[2], OFFSET_THRESH - 1);
            codeSaoMaxUvlc(-ctuParam.offset[3], OFFSET_THRESH - 1);
            if (plane != 2)
                encodeBinsEP((uint32_t)(typeIdx), 2);
        }
    }
}

/** initialize context model with respect to QP and initialization value */
uint8_t sbacInit(int qp, int initValue)
{
    qp = x265_clip3(QP_MIN, QP_MAX_SPEC, qp);

    int  slope      = (initValue >> 4) * 5 - 45;
    int  offset     = ((initValue & 15) << 3) - 16;
    int  initState  =  X265_MIN(X265_MAX(1, (((slope * qp) >> 4) + offset)), 126);
    uint32_t mpState = (initState >= 64);
    uint32_t state = ((mpState ? (initState - 64) : (63 - initState)) << 1) + mpState;

    return (uint8_t)state;
}

static void initBuffer(uint8_t* contextModel, SliceType sliceType, int qp, uint8_t* ctxModel, int size)
{
    ctxModel += sliceType * size;

    for (int n = 0; n < size; n++)
        contextModel[n] = sbacInit(qp, ctxModel[n]);
}

void Entropy::resetEntropy(const Slice& slice)
{
    int  qp              = slice.m_sliceQp;
    SliceType sliceType  = slice.m_sliceType;

    initBuffer(&m_contextState[OFF_SPLIT_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_SPLIT_FLAG, NUM_SPLIT_FLAG_CTX);
    initBuffer(&m_contextState[OFF_SKIP_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_SKIP_FLAG, NUM_SKIP_FLAG_CTX);
    initBuffer(&m_contextState[OFF_MERGE_FLAG_EXT_CTX], sliceType, qp, (uint8_t*)INIT_MERGE_FLAG_EXT, NUM_MERGE_FLAG_EXT_CTX);
    initBuffer(&m_contextState[OFF_MERGE_IDX_EXT_CTX], sliceType, qp, (uint8_t*)INIT_MERGE_IDX_EXT, NUM_MERGE_IDX_EXT_CTX);
    initBuffer(&m_contextState[OFF_PART_SIZE_CTX], sliceType, qp, (uint8_t*)INIT_PART_SIZE, NUM_PART_SIZE_CTX);
    initBuffer(&m_contextState[OFF_PRED_MODE_CTX], sliceType, qp, (uint8_t*)INIT_PRED_MODE, NUM_PRED_MODE_CTX);
    initBuffer(&m_contextState[OFF_ADI_CTX], sliceType, qp, (uint8_t*)INIT_INTRA_PRED_MODE, NUM_ADI_CTX);
    initBuffer(&m_contextState[OFF_CHROMA_PRED_CTX], sliceType, qp, (uint8_t*)INIT_CHROMA_PRED_MODE, NUM_CHROMA_PRED_CTX);
    initBuffer(&m_contextState[OFF_DELTA_QP_CTX], sliceType, qp, (uint8_t*)INIT_DQP, NUM_DELTA_QP_CTX);
    initBuffer(&m_contextState[OFF_INTER_DIR_CTX], sliceType, qp, (uint8_t*)INIT_INTER_DIR, NUM_INTER_DIR_CTX);
    initBuffer(&m_contextState[OFF_REF_NO_CTX], sliceType, qp, (uint8_t*)INIT_REF_PIC, NUM_REF_NO_CTX);
    initBuffer(&m_contextState[OFF_MV_RES_CTX], sliceType, qp, (uint8_t*)INIT_MVD, NUM_MV_RES_CTX);
    initBuffer(&m_contextState[OFF_QT_CBF_CTX], sliceType, qp, (uint8_t*)INIT_QT_CBF, NUM_QT_CBF_CTX);
    initBuffer(&m_contextState[OFF_TRANS_SUBDIV_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_TRANS_SUBDIV_FLAG, NUM_TRANS_SUBDIV_FLAG_CTX);
    initBuffer(&m_contextState[OFF_QT_ROOT_CBF_CTX], sliceType, qp, (uint8_t*)INIT_QT_ROOT_CBF, NUM_QT_ROOT_CBF_CTX);
    initBuffer(&m_contextState[OFF_SIG_CG_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_SIG_CG_FLAG, 2 * NUM_SIG_CG_FLAG_CTX);
    initBuffer(&m_contextState[OFF_SIG_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_SIG_FLAG, NUM_SIG_FLAG_CTX);
    initBuffer(&m_contextState[OFF_CTX_LAST_FLAG_X], sliceType, qp, (uint8_t*)INIT_LAST, NUM_CTX_LAST_FLAG_XY);
    initBuffer(&m_contextState[OFF_CTX_LAST_FLAG_Y], sliceType, qp, (uint8_t*)INIT_LAST, NUM_CTX_LAST_FLAG_XY);
    initBuffer(&m_contextState[OFF_ONE_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_ONE_FLAG, NUM_ONE_FLAG_CTX);
    initBuffer(&m_contextState[OFF_ABS_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_ABS_FLAG, NUM_ABS_FLAG_CTX);
    initBuffer(&m_contextState[OFF_MVP_IDX_CTX], sliceType, qp, (uint8_t*)INIT_MVP_IDX, NUM_MVP_IDX_CTX);
    initBuffer(&m_contextState[OFF_SAO_MERGE_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_SAO_MERGE_FLAG, NUM_SAO_MERGE_FLAG_CTX);
    initBuffer(&m_contextState[OFF_SAO_TYPE_IDX_CTX], sliceType, qp, (uint8_t*)INIT_SAO_TYPE_IDX, NUM_SAO_TYPE_IDX_CTX);
    initBuffer(&m_contextState[OFF_TRANSFORMSKIP_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_TRANSFORMSKIP_FLAG, 2 * NUM_TRANSFORMSKIP_FLAG_CTX);
    initBuffer(&m_contextState[OFF_TQUANT_BYPASS_FLAG_CTX], sliceType, qp, (uint8_t*)INIT_CU_TRANSQUANT_BYPASS_FLAG, NUM_TQUANT_BYPASS_FLAG_CTX);
    // new structure

    start();
}

/* code explicit wp tables */
void Entropy::codePredWeightTable(const Slice& slice)
{
    const WeightParam *wp;
    bool            bChroma      = true; // 4:0:0 not yet supported
    bool            bDenomCoded  = false;
    int             numRefDirs   = slice.m_sliceType == B_SLICE ? 2 : 1;
    uint32_t        totalSignalledWeightFlags = 0;

    if ((slice.m_sliceType == P_SLICE && slice.m_pps->bUseWeightPred) ||
        (slice.m_sliceType == B_SLICE && slice.m_pps->bUseWeightedBiPred))
    {
        for (int list = 0; list < numRefDirs; list++)
        {
            for (int ref = 0; ref < slice.m_numRefIdx[list]; ref++)
            {
                wp = slice.m_weightPredTable[list][ref];
                if (!bDenomCoded)
                {
                    WRITE_UVLC(wp[0].log2WeightDenom, "luma_log2_weight_denom");

                    if (bChroma)
                    {
                        int deltaDenom = wp[1].log2WeightDenom - wp[0].log2WeightDenom;
                        WRITE_SVLC(deltaDenom, "delta_chroma_log2_weight_denom");
                    }
                    bDenomCoded = true;
                }
                WRITE_FLAG(wp[0].bPresentFlag, "luma_weight_lX_flag");
                totalSignalledWeightFlags += wp[0].bPresentFlag;
            }

            if (bChroma)
            {
                for (int ref = 0; ref < slice.m_numRefIdx[list]; ref++)
                {
                    wp = slice.m_weightPredTable[list][ref];
                    WRITE_FLAG(wp[1].bPresentFlag, "chroma_weight_lX_flag");
                    totalSignalledWeightFlags += 2 * wp[1].bPresentFlag;
                }
            }

            for (int ref = 0; ref < slice.m_numRefIdx[list]; ref++)
            {
                wp = slice.m_weightPredTable[list][ref];
                if (wp[0].bPresentFlag)
                {
                    int deltaWeight = (wp[0].inputWeight - (1 << wp[0].log2WeightDenom));
                    WRITE_SVLC(deltaWeight, "delta_luma_weight_lX");
                    WRITE_SVLC(wp[0].inputOffset, "luma_offset_lX");
                }

                if (bChroma)
                {
                    if (wp[1].bPresentFlag)
                    {
                        for (int plane = 1; plane < 3; plane++)
                        {
                            int deltaWeight = (wp[plane].inputWeight - (1 << wp[1].log2WeightDenom));
                            WRITE_SVLC(deltaWeight, "delta_chroma_weight_lX");

                            int pred = (128 - ((128 * wp[plane].inputWeight) >> (wp[plane].log2WeightDenom)));
                            int deltaChroma = (wp[plane].inputOffset - pred);
                            WRITE_SVLC(deltaChroma, "delta_chroma_offset_lX");
                        }
                    }
                }
            }
        }

        X265_CHECK(totalSignalledWeightFlags <= 24, "total weights must be <= 24\n");
    }
}

void Entropy::writeUnaryMaxSymbol(uint32_t symbol, uint8_t* scmModel, int offset, uint32_t maxSymbol)
{
    X265_CHECK(maxSymbol > 0, "maxSymbol too small\n");

    encodeBin(symbol ? 1 : 0, scmModel[0]);

    if (!symbol)
        return;

    bool bCodeLast = (maxSymbol > symbol);

    while (--symbol)
        encodeBin(1, scmModel[offset]);

    if (bCodeLast)
        encodeBin(0, scmModel[offset]);
}

void Entropy::writeEpExGolomb(uint32_t symbol, uint32_t count)
{
    uint32_t bins = 0;
    int numBins = 0;

    while (symbol >= (uint32_t)(1 << count))
    {
        bins = 2 * bins + 1;
        numBins++;
        symbol -= 1 << count;
        count++;
    }

    bins = 2 * bins + 0;
    numBins++;

    bins = (bins << count) | symbol;
    numBins += count;

    X265_CHECK(numBins <= 32, "numBins too large\n");
    encodeBinsEP(bins, numBins);
}

/** Coding of coeff_abs_level_minus3 */
void Entropy::writeCoefRemainExGolomb(uint32_t codeNumber, uint32_t absGoRice)
{
    uint32_t length;
    const uint32_t codeRemain = codeNumber & ((1 << absGoRice) - 1);

    if ((codeNumber >> absGoRice) < COEF_REMAIN_BIN_REDUCTION)
    {
        length = codeNumber >> absGoRice;

        X265_CHECK(codeNumber - (length << absGoRice) == (codeNumber & ((1 << absGoRice) - 1)), "codeNumber failure\n");
        X265_CHECK(length + 1 + absGoRice < 32, "length failure\n");
        encodeBinsEP((((1 << (length + 1)) - 2) << absGoRice) + codeRemain, length + 1 + absGoRice);
    }
    else
    {
        length = 0;
        codeNumber = (codeNumber >> absGoRice) - COEF_REMAIN_BIN_REDUCTION;
        {
            unsigned long idx;
            CLZ(idx, codeNumber + 1);
            length = idx;
            X265_CHECK((codeNumber != 0) || (length == 0), "length check failure\n");
            codeNumber -= (1 << idx) - 1;
        }
        codeNumber = (codeNumber << absGoRice) + codeRemain;

        encodeBinsEP((1 << (COEF_REMAIN_BIN_REDUCTION + length + 1)) - 2, COEF_REMAIN_BIN_REDUCTION + length + 1);
        encodeBinsEP(codeNumber, length + absGoRice);
    }
}

// SBAC RD
void Entropy::loadIntraDirModeLuma(const Entropy& src)
{
    X265_CHECK(src.m_valid, "invalid copy source context\n");
    m_fracBits = src.m_fracBits;
    m_contextState[OFF_ADI_CTX] = src.m_contextState[OFF_ADI_CTX];
}

void Entropy::copyFrom(const Entropy& src)
{
    X265_CHECK(src.m_valid, "invalid copy source context\n");

    copyState(src);

    memcpy(m_contextState, src.m_contextState, MAX_OFF_CTX_MOD * sizeof(uint8_t));
    markValid();
}

void Entropy::codePartSize(const CUData& cu, uint32_t absPartIdx, uint32_t depth)
{
    PartSize partSize = (PartSize)cu.m_partSize[absPartIdx];

    if (cu.isIntra(absPartIdx))
    {
        if (depth == g_maxCUDepth)
            encodeBin(partSize == SIZE_2Nx2N ? 1 : 0, m_contextState[OFF_PART_SIZE_CTX]);
        return;
    }

    switch (partSize)
    {
    case SIZE_2Nx2N:
        encodeBin(1, m_contextState[OFF_PART_SIZE_CTX]);
        break;

    case SIZE_2NxN:
    case SIZE_2NxnU:
    case SIZE_2NxnD:
        encodeBin(0, m_contextState[OFF_PART_SIZE_CTX + 0]);
        encodeBin(1, m_contextState[OFF_PART_SIZE_CTX + 1]);
        if (cu.m_slice->m_sps->maxAMPDepth > depth)
        {
            encodeBin((partSize == SIZE_2NxN) ? 1 : 0, m_contextState[OFF_PART_SIZE_CTX + 3]);
            if (partSize != SIZE_2NxN)
                encodeBinEP((partSize == SIZE_2NxnU ? 0 : 1));
        }
        break;

    case SIZE_Nx2N:
    case SIZE_nLx2N:
    case SIZE_nRx2N:
        encodeBin(0, m_contextState[OFF_PART_SIZE_CTX + 0]);
        encodeBin(0, m_contextState[OFF_PART_SIZE_CTX + 1]);
        if (depth == g_maxCUDepth && !(cu.m_log2CUSize[absPartIdx] == 3))
            encodeBin(1, m_contextState[OFF_PART_SIZE_CTX + 2]);
        if (cu.m_slice->m_sps->maxAMPDepth > depth)
        {
            encodeBin((partSize == SIZE_Nx2N) ? 1 : 0, m_contextState[OFF_PART_SIZE_CTX + 3]);
            if (partSize != SIZE_Nx2N)
                encodeBinEP((partSize == SIZE_nLx2N ? 0 : 1));
        }
        break;
    default:
        X265_CHECK(0, "invalid CU partition\n");
        break;
    }
}

void Entropy::codeMergeIndex(const CUData& cu, uint32_t absPartIdx)
{
    uint32_t numCand = cu.m_slice->m_maxNumMergeCand;

    if (numCand > 1)
    {
        uint32_t unaryIdx = cu.m_mvpIdx[0][absPartIdx]; // merge candidate index was stored in L0 MVP idx 
        encodeBin((unaryIdx != 0), m_contextState[OFF_MERGE_IDX_EXT_CTX]);

        X265_CHECK(unaryIdx < numCand, "unaryIdx out of range\n");

        if (unaryIdx != 0)
        {
            uint32_t mask = (1 << unaryIdx) - 2;
            mask >>= (unaryIdx == numCand - 1) ? 1 : 0;
            encodeBinsEP(mask, unaryIdx - (unaryIdx == numCand - 1));
        }
    }
}

void Entropy::codeIntraDirLumaAng(const CUData& cu, uint32_t absPartIdx, bool isMultiple)
{
    uint32_t dir[4], j;
    uint32_t preds[4][3];
    int predIdx[4];
    uint32_t partNum = isMultiple && cu.m_partSize[absPartIdx] != SIZE_2Nx2N ? 4 : 1;
    uint32_t qNumParts = 1 << (cu.m_log2CUSize[absPartIdx] - 1 - LOG2_UNIT_SIZE) * 2;

    for (j = 0; j < partNum; j++, absPartIdx += qNumParts)
    {
        dir[j] = cu.m_lumaIntraDir[absPartIdx];
        cu.getIntraDirLumaPredictor(absPartIdx, preds[j]);
        predIdx[j] = -1;
        for (uint32_t i = 0; i < 3; i++)
            if (dir[j] == preds[j][i])
                predIdx[j] = i;

        encodeBin((predIdx[j] != -1) ? 1 : 0, m_contextState[OFF_ADI_CTX]);
    }

    for (j = 0; j < partNum; j++)
    {
        if (predIdx[j] != -1)
        {
            X265_CHECK((predIdx[j] >= 0) && (predIdx[j] <= 2), "predIdx out of range\n");
            // NOTE: Mapping
            //       0 = 0
            //       1 = 10
            //       2 = 11
            int nonzero = (!!predIdx[j]);
            encodeBinsEP(predIdx[j] + nonzero, 1 + nonzero);
        }
        else
        {
            if (preds[j][0] > preds[j][1])
                std::swap(preds[j][0], preds[j][1]);

            if (preds[j][0] > preds[j][2])
                std::swap(preds[j][0], preds[j][2]);

            if (preds[j][1] > preds[j][2])
                std::swap(preds[j][1], preds[j][2]);

            dir[j] += (dir[j] > preds[j][2]) ? -1 : 0;
            dir[j] += (dir[j] > preds[j][1]) ? -1 : 0;
            dir[j] += (dir[j] > preds[j][0]) ? -1 : 0;

            encodeBinsEP(dir[j], 5);
        }
    }
}

void Entropy::codeIntraDirChroma(const CUData& cu, uint32_t absPartIdx, uint32_t *chromaDirMode)
{
    uint32_t intraDirChroma = cu.m_chromaIntraDir[absPartIdx];

    if (intraDirChroma == DM_CHROMA_IDX)
        encodeBin(0, m_contextState[OFF_CHROMA_PRED_CTX]);
    else
    {
        for (int i = 0; i < NUM_CHROMA_MODE - 1; i++)
        {
            if (intraDirChroma == chromaDirMode[i])
            {
                intraDirChroma = i;
                break;
            }
        }

        encodeBin(1, m_contextState[OFF_CHROMA_PRED_CTX]);
        encodeBinsEP(intraDirChroma, 2);
    }
}

void Entropy::codeInterDir(const CUData& cu, uint32_t absPartIdx)
{
    const uint32_t interDir = cu.m_interDir[absPartIdx] - 1;
    const uint32_t ctx      = cu.m_cuDepth[absPartIdx]; // the context of the inter dir is the depth of the CU

    if (cu.m_partSize[absPartIdx] == SIZE_2Nx2N || cu.m_log2CUSize[absPartIdx] != 3)
        encodeBin(interDir == 2 ? 1 : 0, m_contextState[OFF_INTER_DIR_CTX + ctx]);
    if (interDir < 2)
        encodeBin(interDir, m_contextState[OFF_INTER_DIR_CTX + 4]);
}

void Entropy::codeRefFrmIdx(const CUData& cu, uint32_t absPartIdx, int list)
{
    uint32_t refFrame = cu.m_refIdx[list][absPartIdx];

    encodeBin(refFrame > 0, m_contextState[OFF_REF_NO_CTX]);

    if (refFrame > 0)
    {
        uint32_t refNum = cu.m_slice->m_numRefIdx[list] - 2;
        if (refNum == 0)
            return;

        refFrame--;
        encodeBin(refFrame > 0, m_contextState[OFF_REF_NO_CTX + 1]);
        if (refFrame > 0)
        {
            uint32_t mask = (1 << refFrame) - 2;
            mask >>= (refFrame == refNum) ? 1 : 0;
            encodeBinsEP(mask, refFrame - (refFrame == refNum));
        }
    }
}

void Entropy::codeMvd(const CUData& cu, uint32_t absPartIdx, int list)
{
    const MV& mvd = cu.m_mvd[list][absPartIdx];
    const int hor = mvd.x;
    const int ver = mvd.y;

    encodeBin(hor != 0 ? 1 : 0, m_contextState[OFF_MV_RES_CTX]);
    encodeBin(ver != 0 ? 1 : 0, m_contextState[OFF_MV_RES_CTX]);

    const bool bHorAbsGr0 = hor != 0;
    const bool bVerAbsGr0 = ver != 0;
    const uint32_t horAbs   = 0 > hor ? -hor : hor;
    const uint32_t verAbs   = 0 > ver ? -ver : ver;

    if (bHorAbsGr0)
        encodeBin(horAbs > 1 ? 1 : 0, m_contextState[OFF_MV_RES_CTX + 1]);

    if (bVerAbsGr0)
        encodeBin(verAbs > 1 ? 1 : 0, m_contextState[OFF_MV_RES_CTX + 1]);

    if (bHorAbsGr0)
    {
        if (horAbs > 1)
            writeEpExGolomb(horAbs - 2, 1);

        encodeBinEP(0 > hor ? 1 : 0);
    }

    if (bVerAbsGr0)
    {
        if (verAbs > 1)
            writeEpExGolomb(verAbs - 2, 1);

        encodeBinEP(0 > ver ? 1 : 0);
    }
}

void Entropy::codeDeltaQP(const CUData& cu, uint32_t absPartIdx)
{
    int dqp = cu.m_qp[absPartIdx] - cu.getRefQP(absPartIdx);

    int qpBdOffsetY = QP_BD_OFFSET;

    dqp = (dqp + 78 + qpBdOffsetY + (qpBdOffsetY / 2)) % (52 + qpBdOffsetY) - 26 - (qpBdOffsetY / 2);

    uint32_t absDQp = (uint32_t)((dqp > 0) ? dqp  : (-dqp));
    uint32_t TUValue = X265_MIN((int)absDQp, CU_DQP_TU_CMAX);
    writeUnaryMaxSymbol(TUValue, &m_contextState[OFF_DELTA_QP_CTX], 1, CU_DQP_TU_CMAX);
    if (absDQp >= CU_DQP_TU_CMAX)
        writeEpExGolomb(absDQp - CU_DQP_TU_CMAX, CU_DQP_EG_k);

    if (absDQp > 0)
    {
        uint32_t sign = (dqp > 0 ? 0 : 1);
        encodeBinEP(sign);
    }
}

void Entropy::codeQtCbfChroma(const CUData& cu, uint32_t absPartIdx, TextType ttype, uint32_t tuDepth, bool lowestLevel)
{
    uint32_t ctx = tuDepth + 2;

    uint32_t log2TrSize = cu.m_log2CUSize[absPartIdx] - tuDepth;
    bool canQuadSplit       = (log2TrSize - cu.m_hChromaShift > 2);
    uint32_t lowestTUDepth  = tuDepth + ((!lowestLevel && !canQuadSplit) ? 1 : 0); // unsplittable TUs inherit their parent's CBF

    if (cu.m_chromaFormat == X265_CSP_I422 && (lowestLevel || !canQuadSplit)) // if sub-TUs are present
    {
        uint32_t subTUDepth        = lowestTUDepth + 1;   // if this is the lowest level of the TU-tree, the sub-TUs are directly below.
                                                          // Otherwise, this must be the level above the lowest level (as specified above)
        uint32_t tuNumParts = 1 << ((log2TrSize - LOG2_UNIT_SIZE) * 2 - 1);

        encodeBin(cu.getCbf(absPartIdx             , ttype, subTUDepth), m_contextState[OFF_QT_CBF_CTX + ctx]);
        encodeBin(cu.getCbf(absPartIdx + tuNumParts, ttype, subTUDepth), m_contextState[OFF_QT_CBF_CTX + ctx]);
    }
    else
        encodeBin(cu.getCbf(absPartIdx, ttype, lowestTUDepth), m_contextState[OFF_QT_CBF_CTX + ctx]);
}

void Entropy::codeCoeffNxN(const CUData& cu, const coeff_t* coeff, uint32_t absPartIdx, uint32_t log2TrSize, TextType ttype)
{
    uint32_t trSize = 1 << log2TrSize;
    uint32_t tqBypass = cu.m_tqBypass[absPartIdx];
    // compute number of significant coefficients
    uint32_t numSig = primitives.cu[log2TrSize - 2].count_nonzero(coeff);
    X265_CHECK(numSig > 0, "cbf check fail\n");
    bool bHideFirstSign = cu.m_slice->m_pps->bSignHideEnabled && !tqBypass;

    if (log2TrSize <= MAX_LOG2_TS_SIZE && !tqBypass && cu.m_slice->m_pps->bTransformSkipEnabled)
        codeTransformSkipFlags(cu.m_transformSkip[ttype][absPartIdx], ttype);

    bool bIsLuma = ttype == TEXT_LUMA;

    // select scans
    TUEntropyCodingParameters codingParameters;
    cu.getTUEntropyCodingParameters(codingParameters, absPartIdx, log2TrSize, bIsLuma);

    uint8_t coeffNum[MLS_GRP_NUM];      // value range[0, 16]
    uint16_t coeffSign[MLS_GRP_NUM];    // bit mask map for non-zero coeff sign
    uint16_t coeffFlag[MLS_GRP_NUM];    // bit mask map for non-zero coeff

    //----- encode significance map -----

    // Find position of last coefficient
    int scanPosLast = 0;
    uint32_t posLast;
    uint64_t sigCoeffGroupFlag64 = 0;
    //const uint32_t maskPosXY = ((uint32_t)~0 >> (31 - log2TrSize + MLS_CG_LOG2_SIZE)) >> 1;
    X265_CHECK((uint32_t)((1 << (log2TrSize - MLS_CG_LOG2_SIZE)) - 1) == (((uint32_t)~0 >> (31 - log2TrSize + MLS_CG_LOG2_SIZE)) >> 1), "maskPosXY fault\n");

    scanPosLast = primitives.scanPosLast(codingParameters.scan, coeff, coeffSign, coeffFlag, coeffNum, numSig, g_scan4x4[codingParameters.scanType], trSize);
    posLast = codingParameters.scan[scanPosLast];

    const int lastScanSet = scanPosLast >> MLS_CG_SIZE;

    // Calculate CG block non-zero mask, the latest CG always flag as non-zero in CG scan loop
    for(int idx = 0; idx < lastScanSet; idx++)
    {
        const uint8_t subSet = (uint8_t)codingParameters.scanCG[idx];
        const uint8_t nonZero = (coeffNum[idx] != 0);
        sigCoeffGroupFlag64 |= ((nonZero ? (uint64_t)1 : 0) << subSet);
    }


    // Code position of last coefficient
    {
        // The last position is composed of a prefix and suffix.
        // The prefix is context coded truncated unary bins. The suffix is bypass coded fixed length bins.
        // The bypass coded bins for both the x and y components are grouped together.
        uint32_t packedSuffixBits = 0, packedSuffixLen = 0;
        uint32_t pos[2] = { (posLast & (trSize - 1)), (posLast >> log2TrSize) };
        // swap
        if (codingParameters.scanType == SCAN_VER)
            std::swap(pos[0], pos[1]);

        int ctxIdx = bIsLuma ? (3 * (log2TrSize - 2) + ((log2TrSize - 1) >> 2)) : NUM_CTX_LAST_FLAG_XY_LUMA;
        int ctxShift = bIsLuma ? ((log2TrSize + 1) >> 2) : log2TrSize - 2;
        uint32_t maxGroupIdx = (log2TrSize << 1) - 1;

        uint8_t *ctx = &m_contextState[OFF_CTX_LAST_FLAG_X];
        for (uint32_t i = 0; i < 2; i++, ctxIdx += NUM_CTX_LAST_FLAG_XY)
        {
            uint32_t temp = g_lastCoeffTable[pos[i]];
            uint32_t prefixOnes = temp & 15;
            uint32_t suffixLen = temp >> 4;

            for (uint32_t ctxLast = 0; ctxLast < prefixOnes; ctxLast++)
                encodeBin(1, *(ctx + ctxIdx + (ctxLast >> ctxShift)));

            if (prefixOnes < maxGroupIdx)
                encodeBin(0, *(ctx + ctxIdx + (prefixOnes >> ctxShift)));

            packedSuffixBits <<= suffixLen;
            packedSuffixBits |= (pos[i] & ((1 << suffixLen) - 1));
            packedSuffixLen += suffixLen;
        }

        encodeBinsEP(packedSuffixBits, packedSuffixLen);
    }

    // code significance flag
    uint8_t * const baseCoeffGroupCtx = &m_contextState[OFF_SIG_CG_FLAG_CTX + (bIsLuma ? 0 : NUM_SIG_CG_FLAG_CTX)];
    uint8_t * const baseCtx = bIsLuma ? &m_contextState[OFF_SIG_FLAG_CTX] : &m_contextState[OFF_SIG_FLAG_CTX + NUM_SIG_FLAG_CTX_LUMA];
    uint32_t c1 = 1;
    int scanPosSigOff = scanPosLast - (lastScanSet << MLS_CG_SIZE) - 1;
    int absCoeff[1 << MLS_CG_SIZE];
    uint32_t numNonZero = 1;
    unsigned long lastNZPosInCG;
    unsigned long firstNZPosInCG;

    absCoeff[0] = int(abs(coeff[posLast]));

    for (int subSet = lastScanSet; subSet >= 0; subSet--)
    {
        const uint32_t subCoeffFlag = coeffFlag[subSet];
        uint32_t scanFlagMask = subCoeffFlag;
        int subPosBase = subSet << MLS_CG_SIZE;
        
        if (subSet == lastScanSet)
        {
            X265_CHECK(scanPosSigOff == scanPosLast - (lastScanSet << MLS_CG_SIZE) - 1, "scanPos mistake\n");
            scanFlagMask >>= 1;
        }

        // encode significant_coeffgroup_flag
        const int cgBlkPos = codingParameters.scanCG[subSet];
        const int cgPosY   = cgBlkPos >> (log2TrSize - MLS_CG_LOG2_SIZE);
        const int cgPosX   = cgBlkPos & ((1 << (log2TrSize - MLS_CG_LOG2_SIZE)) - 1);
        const uint64_t cgBlkPosMask = ((uint64_t)1 << cgBlkPos);

        if (subSet == lastScanSet || !subSet)
            sigCoeffGroupFlag64 |= cgBlkPosMask;
        else
        {
            uint32_t sigCoeffGroup = ((sigCoeffGroupFlag64 & cgBlkPosMask) != 0);
            uint32_t ctxSig = Quant::getSigCoeffGroupCtxInc(sigCoeffGroupFlag64, cgPosX, cgPosY, cgBlkPos, (trSize >> MLS_CG_LOG2_SIZE));
            encodeBin(sigCoeffGroup, baseCoeffGroupCtx[ctxSig]);
        }

        // encode significant_coeff_flag
        if ((scanPosSigOff >= 0) && (sigCoeffGroupFlag64 & cgBlkPosMask))
        {
            X265_CHECK((log2TrSize != 2) || (log2TrSize == 2 && subSet == 0), "log2TrSize and subSet mistake!\n");
            const int patternSigCtx = Quant::calcPatternSigCtx(sigCoeffGroupFlag64, cgPosX, cgPosY, cgBlkPos, (trSize >> MLS_CG_LOG2_SIZE));
            const uint32_t posOffset = (bIsLuma && subSet) ? 3 : 0;

            static const uint8_t ctxIndMap4x4[16] =
            {
                0, 1, 4, 5,
                2, 3, 4, 5,
                6, 6, 8, 8,
                7, 7, 8, 8
            };
            // NOTE: [patternSigCtx][posXinSubset][posYinSubset]
            static const uint8_t table_cnt[4][SCAN_SET_SIZE] =
            {
                // patternSigCtx = 0
                {
                    2, 1, 1, 0,
                    1, 1, 0, 0,
                    1, 0, 0, 0,
                    0, 0, 0, 0,
                },
                // patternSigCtx = 1
                {
                    2, 2, 2, 2,
                    1, 1, 1, 1,
                    0, 0, 0, 0,
                    0, 0, 0, 0,
                },
                // patternSigCtx = 2
                {
                    2, 1, 0, 0,
                    2, 1, 0, 0,
                    2, 1, 0, 0,
                    2, 1, 0, 0,
                },
                // patternSigCtx = 3
                {
                    2, 2, 2, 2,
                    2, 2, 2, 2,
                    2, 2, 2, 2,
                    2, 2, 2, 2,
                }
            };

            const int offset = codingParameters.firstSignificanceMapContext;
            ALIGN_VAR_32(uint16_t, tmpCoeff[SCAN_SET_SIZE]);
            // TODO: accelerate by PABSW
            const uint32_t blkPosBase  = codingParameters.scan[subPosBase];
            for (int i = 0; i < MLS_CG_SIZE; i++)
            {
                tmpCoeff[i * MLS_CG_SIZE + 0] = (uint16_t)abs(coeff[blkPosBase + i * trSize + 0]);
                tmpCoeff[i * MLS_CG_SIZE + 1] = (uint16_t)abs(coeff[blkPosBase + i * trSize + 1]);
                tmpCoeff[i * MLS_CG_SIZE + 2] = (uint16_t)abs(coeff[blkPosBase + i * trSize + 2]);
                tmpCoeff[i * MLS_CG_SIZE + 3] = (uint16_t)abs(coeff[blkPosBase + i * trSize + 3]);
            }

            X265_CHECK(scanPosSigOff >= 0, "scanPosSigOff check failure\n");
            if (m_bitIf)
            {
                if (log2TrSize == 2)
                {
                    do
                    {
                        uint32_t blkPos, sig, ctxSig;
                        blkPos = g_scan4x4[codingParameters.scanType][scanPosSigOff];
                        sig     = scanFlagMask & 1;
                        scanFlagMask >>= 1;
                        X265_CHECK((uint32_t)(tmpCoeff[blkPos] != 0) == sig, "sign bit mistake\n");
                        {
                            ctxSig = ctxIndMap4x4[blkPos];
                            X265_CHECK(ctxSig == Quant::getSigCtxInc(patternSigCtx, log2TrSize, trSize, blkPos, bIsLuma, codingParameters.firstSignificanceMapContext), "sigCtx mistake!\n");;
                            encodeBin(sig, baseCtx[ctxSig]);
                        }
                        absCoeff[numNonZero] = tmpCoeff[blkPos];
                        numNonZero += sig;
                        scanPosSigOff--;
                    }
                    while(scanPosSigOff >= 0);
                }
                else
                {
                    X265_CHECK((log2TrSize > 2), "log2TrSize must be more than 2 in this path!\n");

                    const uint8_t *tabSigCtx = table_cnt[(uint32_t)patternSigCtx];
                    do
                    {
                        uint32_t blkPos, sig, ctxSig;
                        blkPos = g_scan4x4[codingParameters.scanType][scanPosSigOff];
                        const uint32_t posZeroMask = (subPosBase + scanPosSigOff) ? ~0 : 0;
                        sig     = scanFlagMask & 1;
                        scanFlagMask >>= 1;
                        X265_CHECK((uint32_t)(tmpCoeff[blkPos] != 0) == sig, "sign bit mistake\n");
                        if (scanPosSigOff != 0 || subSet == 0 || numNonZero)
                        {
                            const uint32_t cnt = tabSigCtx[blkPos] + offset;
                            ctxSig = (cnt + posOffset) & posZeroMask;

                            X265_CHECK(ctxSig == Quant::getSigCtxInc(patternSigCtx, log2TrSize, trSize, codingParameters.scan[subPosBase + scanPosSigOff], bIsLuma, codingParameters.firstSignificanceMapContext), "sigCtx mistake!\n");;
                            encodeBin(sig, baseCtx[ctxSig]);
                        }
                        absCoeff[numNonZero] = tmpCoeff[blkPos];
                        numNonZero += sig;
                        scanPosSigOff--;
                    }
                    while(scanPosSigOff >= 0);
                }
            }
            else // fast RD path
            {
                // maximum g_entropyBits are 18-bits and maximum of count are 16, so intermedia of sum are 22-bits
                uint32_t sum = 0;
                if (log2TrSize == 2)
                {
                    do
                    {
                        uint32_t blkPos, sig, ctxSig;
                        blkPos = g_scan4x4[codingParameters.scanType][scanPosSigOff];
                        sig     = scanFlagMask & 1;
                        scanFlagMask >>= 1;
                        X265_CHECK((uint32_t)(tmpCoeff[blkPos] != 0) == sig, "sign bit mistake\n");
                        {
                            ctxSig = ctxIndMap4x4[blkPos];
                            X265_CHECK(ctxSig == Quant::getSigCtxInc(patternSigCtx, log2TrSize, trSize, codingParameters.scan[subPosBase + scanPosSigOff], bIsLuma, codingParameters.firstSignificanceMapContext), "sigCtx mistake!\n");;
                            //encodeBin(sig, baseCtx[ctxSig]);
                            const uint32_t mstate = baseCtx[ctxSig];
                            const uint32_t mps = mstate & 1;
                            const uint32_t stateBits = g_entropyStateBits[mstate ^ sig];
                            uint32_t nextState = (stateBits >> 23) + mps;
                            if ((mstate ^ sig) == 1)
                                nextState = sig;
                            X265_CHECK(sbacNext(mstate, sig) == nextState, "nextState check failure\n");
                            X265_CHECK(sbacGetEntropyBits(mstate, sig) == (stateBits & 0xFFFFFF), "entropyBits check failure\n");
                            baseCtx[ctxSig] = (uint8_t)nextState;
                            sum += stateBits;
                        }
                        absCoeff[numNonZero] = tmpCoeff[blkPos];
                        numNonZero += sig;
                        scanPosSigOff--;
                    }
                    while(scanPosSigOff >= 0);
                } // end of 4x4
                else
                {
                    X265_CHECK((log2TrSize > 2), "log2TrSize must be more than 2 in this path!\n");

                    const uint8_t *tabSigCtx = table_cnt[(uint32_t)patternSigCtx];
                    do
                    {
                        uint32_t blkPos, sig, ctxSig;
                        blkPos = g_scan4x4[codingParameters.scanType][scanPosSigOff];
                        const uint32_t posZeroMask = (subPosBase + scanPosSigOff) ? ~0 : 0;
                        sig     = scanFlagMask & 1;
                        scanFlagMask >>= 1;
                        X265_CHECK((uint32_t)(tmpCoeff[blkPos] != 0) == sig, "sign bit mistake\n");
                        if (scanPosSigOff != 0 || subSet == 0 || numNonZero)
                        {
                            const uint32_t cnt = tabSigCtx[blkPos] + offset;
                            ctxSig = (cnt + posOffset) & posZeroMask;

                            X265_CHECK(ctxSig == Quant::getSigCtxInc(patternSigCtx, log2TrSize, trSize, codingParameters.scan[subPosBase + scanPosSigOff], bIsLuma, codingParameters.firstSignificanceMapContext), "sigCtx mistake!\n");;
                            //encodeBin(sig, baseCtx[ctxSig]);
                            const uint32_t mstate = baseCtx[ctxSig];
                            const uint32_t mps = mstate & 1;
                            const uint32_t stateBits = g_entropyStateBits[mstate ^ sig];
                            uint32_t nextState = (stateBits >> 23) + mps;
                            if ((mstate ^ sig) == 1)
                                nextState = sig;
                            X265_CHECK(sbacNext(mstate, sig) == nextState, "nextState check failure\n");
                            X265_CHECK(sbacGetEntropyBits(mstate, sig) == (stateBits & 0xFFFFFF), "entropyBits check failure\n");
                            baseCtx[ctxSig] = (uint8_t)nextState;
                            sum += stateBits;
                        }
                        absCoeff[numNonZero] = tmpCoeff[blkPos];
                        numNonZero += sig;
                        scanPosSigOff--;
                    }
                    while(scanPosSigOff >= 0);
                } // end of non 4x4 path
                sum &= 0xFFFFFF;

                // update RD cost
                m_fracBits += sum;
            } // end of fast RD path -- !m_bitIf
        }
        X265_CHECK(coeffNum[subSet] == numNonZero, "coefNum mistake\n");

        uint32_t coeffSigns = coeffSign[subSet];
        numNonZero = coeffNum[subSet];
        if (numNonZero > 0)
        {
            uint32_t idx;
            X265_CHECK(subCoeffFlag > 0, "subCoeffFlag is zero\n");
            CLZ(lastNZPosInCG, subCoeffFlag);
            CTZ(firstNZPosInCG, subCoeffFlag);

            bool signHidden = (lastNZPosInCG - firstNZPosInCG >= SBH_THRESHOLD);
            uint32_t ctxSet = (subSet > 0 && bIsLuma) ? 2 : 0;

            if (c1 == 0)
                ctxSet++;

            c1 = 1;
            uint8_t *baseCtxMod = bIsLuma ? &m_contextState[OFF_ONE_FLAG_CTX + 4 * ctxSet] : &m_contextState[OFF_ONE_FLAG_CTX + NUM_ONE_FLAG_CTX_LUMA + 4 * ctxSet];

            uint32_t numC1Flag = X265_MIN(numNonZero, C1FLAG_NUMBER);
            int firstC2FlagIdx = -1;

            X265_CHECK(numC1Flag > 0, "numC1Flag check failure\n");
            idx = 0;
            do
            {
                uint32_t symbol = absCoeff[idx] > 1;
                encodeBin(symbol, baseCtxMod[c1]);
                if (symbol)
                {
                    c1 = 0;

                    if (firstC2FlagIdx == -1)
                        firstC2FlagIdx = idx;
                }
                else if ((c1 < 3) && (c1 > 0))
                    c1++;
                idx++;
            }
            while(idx < numC1Flag);

            if (!c1)
            {
                baseCtxMod = bIsLuma ? &m_contextState[OFF_ABS_FLAG_CTX + ctxSet] : &m_contextState[OFF_ABS_FLAG_CTX + NUM_ABS_FLAG_CTX_LUMA + ctxSet];

                X265_CHECK((firstC2FlagIdx != -1), "firstC2FlagIdx check failure\n");
                uint32_t symbol = absCoeff[firstC2FlagIdx] > 2;
                encodeBin(symbol, baseCtxMod[0]);
            }

            const int hiddenShift = (bHideFirstSign && signHidden) ? 1 : 0;
            encodeBinsEP((coeffSigns >> hiddenShift), numNonZero - hiddenShift);

            if (!c1 || numNonZero > C1FLAG_NUMBER)
            {
                uint32_t goRiceParam = 0;
                int firstCoeff2 = 1;
                uint32_t baseLevelN = 0x5555AAAA; // 2-bits encode format baseLevel

                if (!m_bitIf)
                {
                    // FastRd path
                    idx = 0;
                    do
                    {
                        int baseLevel = (baseLevelN & 3) | firstCoeff2;
                        X265_CHECK(baseLevel == ((idx < C1FLAG_NUMBER) ? (2 + firstCoeff2) : 1), "baseLevel check failurr\n");
                        baseLevelN >>= 2;
                        int codeNumber = absCoeff[idx] - baseLevel;

                        if (codeNumber >= 0)
                        {
                            //writeCoefRemainExGolomb(absCoeff[idx] - baseLevel, goRiceParam);
                            uint32_t length = 0;

                            codeNumber = ((uint32_t)codeNumber >> goRiceParam) - COEF_REMAIN_BIN_REDUCTION;
                            if (codeNumber >= 0)
                            {
                                {
                                    unsigned long cidx;
                                    CLZ(cidx, codeNumber + 1);
                                    length = cidx;
                                }
                                X265_CHECK((codeNumber != 0) || (length == 0), "length check failure\n");

                                codeNumber = (length + length);
                            }
                            m_fracBits += (COEF_REMAIN_BIN_REDUCTION + 1 + goRiceParam + codeNumber) << 15;

                            if (absCoeff[idx] > (COEF_REMAIN_BIN_REDUCTION << goRiceParam))
                                goRiceParam = (goRiceParam + 1) - (goRiceParam >> 2);
                            X265_CHECK(goRiceParam <= 4, "goRiceParam check failure\n");
                        }
                        if (absCoeff[idx] >= 2)
                            firstCoeff2 = 0;
                        idx++;
                    }
                    while(idx < numNonZero);
                }
                else
                {
                    // Standard path
                    idx = 0;
                    do
                    {
                        int baseLevel = (baseLevelN & 3) | firstCoeff2;
                        X265_CHECK(baseLevel == ((idx < C1FLAG_NUMBER) ? (2 + firstCoeff2) : 1), "baseLevel check failurr\n");
                        baseLevelN >>= 2;

                        if (absCoeff[idx] >= baseLevel)
                        {
                            writeCoefRemainExGolomb(absCoeff[idx] - baseLevel, goRiceParam);
                            if (absCoeff[idx] > (COEF_REMAIN_BIN_REDUCTION << goRiceParam))
                                goRiceParam = (goRiceParam + 1) - (goRiceParam >> 2);
                            X265_CHECK(goRiceParam <= 4, "goRiceParam check failure\n");
                        }
                        if (absCoeff[idx] >= 2)
                            firstCoeff2 = 0;
                        idx++;
                    }
                    while(idx < numNonZero);
                }
            }
        }
        // Initialize value for next loop
        numNonZero = 0;
        scanPosSigOff = (1 << MLS_CG_SIZE) - 1;
    }
}

void Entropy::codeSaoMaxUvlc(uint32_t code, uint32_t maxSymbol)
{
    X265_CHECK(maxSymbol > 0, "maxSymbol too small\n");

    uint32_t isCodeNonZero = !!code;

    encodeBinEP(isCodeNonZero);
    if (isCodeNonZero)
    {
        uint32_t isCodeLast = (maxSymbol > code);
        uint32_t mask = (1 << (code - 1)) - 1;
        uint32_t len = code - 1 + isCodeLast;
        mask <<= isCodeLast;

        encodeBinsEP(mask, len);
    }
}

/* estimate bit cost for CBP, significant map and significant coefficients */
void Entropy::estBit(EstBitsSbac& estBitsSbac, uint32_t log2TrSize, bool bIsLuma) const
{
    estCBFBit(estBitsSbac);

    estSignificantCoeffGroupMapBit(estBitsSbac, bIsLuma);

    // encode significance map
    estSignificantMapBit(estBitsSbac, log2TrSize, bIsLuma);

    // encode significant coefficients
    estSignificantCoefficientsBit(estBitsSbac, bIsLuma);
}

/* estimate bit cost for each CBP bit */
void Entropy::estCBFBit(EstBitsSbac& estBitsSbac) const
{
    const uint8_t *ctx = &m_contextState[OFF_QT_CBF_CTX];

    for (uint32_t ctxInc = 0; ctxInc < NUM_QT_CBF_CTX; ctxInc++)
    {
        estBitsSbac.blockCbpBits[ctxInc][0] = sbacGetEntropyBits(ctx[ctxInc], 0);
        estBitsSbac.blockCbpBits[ctxInc][1] = sbacGetEntropyBits(ctx[ctxInc], 1);
    }

    ctx = &m_contextState[OFF_QT_ROOT_CBF_CTX];

    estBitsSbac.blockRootCbpBits[0] = sbacGetEntropyBits(ctx[0], 0);
    estBitsSbac.blockRootCbpBits[1] = sbacGetEntropyBits(ctx[0], 1);
}

/* estimate SAMBAC bit cost for significant coefficient group map */
void Entropy::estSignificantCoeffGroupMapBit(EstBitsSbac& estBitsSbac, bool bIsLuma) const
{
    int firstCtx = 0, numCtx = NUM_SIG_CG_FLAG_CTX;

    for (int ctxIdx = firstCtx; ctxIdx < firstCtx + numCtx; ctxIdx++)
        for (uint32_t bin = 0; bin < 2; bin++)
            estBitsSbac.significantCoeffGroupBits[ctxIdx][bin] = sbacGetEntropyBits(m_contextState[OFF_SIG_CG_FLAG_CTX + ((bIsLuma ? 0 : NUM_SIG_CG_FLAG_CTX) + ctxIdx)], bin);
}

/* estimate SAMBAC bit cost for significant coefficient map */
void Entropy::estSignificantMapBit(EstBitsSbac& estBitsSbac, uint32_t log2TrSize, bool bIsLuma) const
{
    int firstCtx = 1, numCtx = 8;

    if (log2TrSize >= 4)
    {
        firstCtx = bIsLuma ? 21 : 12;
        numCtx = bIsLuma ? 6 : 3;
    }
    else if (log2TrSize == 3)
    {
        firstCtx = 9;
        numCtx = bIsLuma ? 12 : 3;
    }

    if (bIsLuma)
    {
        for (uint32_t bin = 0; bin < 2; bin++)
            estBitsSbac.significantBits[bin][0] = sbacGetEntropyBits(m_contextState[OFF_SIG_FLAG_CTX], bin);

        for (int ctxIdx = firstCtx; ctxIdx < firstCtx + numCtx; ctxIdx++)
            for (uint32_t bin = 0; bin < 2; bin++)
                estBitsSbac.significantBits[bin][ctxIdx] = sbacGetEntropyBits(m_contextState[OFF_SIG_FLAG_CTX + ctxIdx], bin);
    }
    else
    {
        for (uint32_t bin = 0; bin < 2; bin++)
            estBitsSbac.significantBits[bin][0] = sbacGetEntropyBits(m_contextState[OFF_SIG_FLAG_CTX + (NUM_SIG_FLAG_CTX_LUMA + 0)], bin);

        for (int ctxIdx = firstCtx; ctxIdx < firstCtx + numCtx; ctxIdx++)
            for (uint32_t bin = 0; bin < 2; bin++)
                estBitsSbac.significantBits[bin][ctxIdx] = sbacGetEntropyBits(m_contextState[OFF_SIG_FLAG_CTX + (NUM_SIG_FLAG_CTX_LUMA + ctxIdx)], bin);
    }

    int blkSizeOffset = bIsLuma ? ((log2TrSize - 2) * 3 + ((log2TrSize - 1) >> 2)) : NUM_CTX_LAST_FLAG_XY_LUMA;
    int ctxShift = bIsLuma ? ((log2TrSize + 1) >> 2) : log2TrSize - 2;
    uint32_t maxGroupIdx = log2TrSize * 2 - 1;

    uint32_t ctx;
    for (int i = 0, ctxIdx = 0; i < 2; i++, ctxIdx += NUM_CTX_LAST_FLAG_XY)
    {
        int bits = 0;
        const uint8_t *ctxState = &m_contextState[OFF_CTX_LAST_FLAG_X + ctxIdx];

        for (ctx = 0; ctx < maxGroupIdx; ctx++)
        {
            int ctxOffset = blkSizeOffset + (ctx >> ctxShift);
            estBitsSbac.lastBits[i][ctx] = bits + sbacGetEntropyBits(ctxState[ctxOffset], 0);
            bits += sbacGetEntropyBits(ctxState[ctxOffset], 1);
        }

        estBitsSbac.lastBits[i][ctx] = bits;
    }
}

/* estimate bit cost of significant coefficient */
void Entropy::estSignificantCoefficientsBit(EstBitsSbac& estBitsSbac, bool bIsLuma) const
{
    if (bIsLuma)
    {
        const uint8_t *ctxOne = &m_contextState[OFF_ONE_FLAG_CTX];
        const uint8_t *ctxAbs = &m_contextState[OFF_ABS_FLAG_CTX];

        for (int ctxIdx = 0; ctxIdx < NUM_ONE_FLAG_CTX_LUMA; ctxIdx++)
        {
            estBitsSbac.greaterOneBits[ctxIdx][0] = sbacGetEntropyBits(ctxOne[ctxIdx], 0);
            estBitsSbac.greaterOneBits[ctxIdx][1] = sbacGetEntropyBits(ctxOne[ctxIdx], 1);
        }

        for (int ctxIdx = 0; ctxIdx < NUM_ABS_FLAG_CTX_LUMA; ctxIdx++)
        {
            estBitsSbac.levelAbsBits[ctxIdx][0] = sbacGetEntropyBits(ctxAbs[ctxIdx], 0);
            estBitsSbac.levelAbsBits[ctxIdx][1] = sbacGetEntropyBits(ctxAbs[ctxIdx], 1);
        }
    }
    else
    {
        const uint8_t *ctxOne = &m_contextState[OFF_ONE_FLAG_CTX + NUM_ONE_FLAG_CTX_LUMA];
        const uint8_t *ctxAbs = &m_contextState[OFF_ABS_FLAG_CTX + NUM_ABS_FLAG_CTX_LUMA];

        for (int ctxIdx = 0; ctxIdx < NUM_ONE_FLAG_CTX_CHROMA; ctxIdx++)
        {
            estBitsSbac.greaterOneBits[ctxIdx][0] = sbacGetEntropyBits(ctxOne[ctxIdx], 0);
            estBitsSbac.greaterOneBits[ctxIdx][1] = sbacGetEntropyBits(ctxOne[ctxIdx], 1);
        }

        for (int ctxIdx = 0; ctxIdx < NUM_ABS_FLAG_CTX_CHROMA; ctxIdx++)
        {
            estBitsSbac.levelAbsBits[ctxIdx][0] = sbacGetEntropyBits(ctxAbs[ctxIdx], 0);
            estBitsSbac.levelAbsBits[ctxIdx][1] = sbacGetEntropyBits(ctxAbs[ctxIdx], 1);
        }
    }
}

/* Initialize our context information from the nominated source */
void Entropy::copyContextsFrom(const Entropy& src)
{
    X265_CHECK(src.m_valid, "invalid copy source context\n");

    memcpy(m_contextState, src.m_contextState, MAX_OFF_CTX_MOD * sizeof(m_contextState[0]));
    markValid();
}

void Entropy::start()
{
    m_low = 0;
    m_range = 510;
    m_bitsLeft = -12;
    m_numBufferedBytes = 0;
    m_bufferedByte = 0xff;
}

void Entropy::finish()
{
    if (m_low >> (21 + m_bitsLeft))
    {
        m_bitIf->writeByte(m_bufferedByte + 1);
        while (m_numBufferedBytes > 1)
        {
            m_bitIf->writeByte(0x00);
            m_numBufferedBytes--;
        }

        m_low -= 1 << (21 + m_bitsLeft);
    }
    else
    {
        if (m_numBufferedBytes > 0)
            m_bitIf->writeByte(m_bufferedByte);

        while (m_numBufferedBytes > 1)
        {
            m_bitIf->writeByte(0xff);
            m_numBufferedBytes--;
        }
    }
    m_bitIf->write(m_low >> 8, 13 + m_bitsLeft);
}

void Entropy::copyState(const Entropy& other)
{
    m_low = other.m_low;
    m_range = other.m_range;
    m_bitsLeft = other.m_bitsLeft;
    m_bufferedByte = other.m_bufferedByte;
    m_numBufferedBytes = other.m_numBufferedBytes;
    m_fracBits = other.m_fracBits;
}

void Entropy::resetBits()
{
    m_low = 0;
    m_bitsLeft = -12;
    m_numBufferedBytes = 0;
    m_bufferedByte = 0xff;
    m_fracBits &= 32767;
    if (m_bitIf)
        m_bitIf->resetBits();
}

/** Encode bin */
void Entropy::encodeBin(uint32_t binValue, uint8_t &ctxModel)
{
    uint32_t mstate = ctxModel;

    ctxModel = sbacNext(mstate, binValue);

    if (!m_bitIf)
    {
        m_fracBits += sbacGetEntropyBits(mstate, binValue);
        return;
    }

    uint32_t range = m_range;
    uint32_t state = sbacGetState(mstate);
    uint32_t lps = g_lpsTable[state][((uint8_t)range >> 6)];
    range -= lps;

    X265_CHECK(lps >= 2, "lps is too small\n");

    int numBits = (uint32_t)(range - 256) >> 31;
    uint32_t low = m_low;

    // NOTE: MPS must be LOWEST bit in mstate
    X265_CHECK((uint32_t)((binValue ^ mstate) & 1) == (uint32_t)(binValue != sbacGetMps(mstate)), "binValue failure\n");
    if ((binValue ^ mstate) & 1)
    {
        // NOTE: lps is non-zero and the maximum of idx is 8 because lps less than 256
        //numBits = g_renormTable[lps >> 3];
        unsigned long idx;
        CLZ(idx, lps);
        X265_CHECK(state != 63 || idx == 1, "state failure\n");

        numBits = 8 - idx;
        if (state >= 63)
            numBits = 6;
        X265_CHECK(numBits <= 6, "numBits failure\n");

        low += range;
        range = lps;
    }
    m_low = (low << numBits);
    m_range = (range << numBits);
    m_bitsLeft += numBits;

    if (m_bitsLeft >= 0)
        writeOut();
}

/** Encode equiprobable bin */
void Entropy::encodeBinEP(uint32_t binValue)
{
    if (!m_bitIf)
    {
        m_fracBits += 32768;
        return;
    }
    m_low <<= 1;
    if (binValue)
        m_low += m_range;
    m_bitsLeft++;

    if (m_bitsLeft >= 0)
        writeOut();
}

/** Encode equiprobable bins */
void Entropy::encodeBinsEP(uint32_t binValues, int numBins)
{
    if (!m_bitIf)
    {
        m_fracBits += 32768 * numBins;
        return;
    }

    while (numBins > 8)
    {
        numBins -= 8;
        uint32_t pattern = binValues >> numBins;
        m_low <<= 8;
        m_low += m_range * pattern;
        binValues -= pattern << numBins;
        m_bitsLeft += 8;

        if (m_bitsLeft >= 0)
            writeOut();
    }

    m_low <<= numBins;
    m_low += m_range * binValues;
    m_bitsLeft += numBins;

    if (m_bitsLeft >= 0)
        writeOut();
}

/** Encode terminating bin */
void Entropy::encodeBinTrm(uint32_t binValue)
{
    if (!m_bitIf)
    {
        m_fracBits += sbacGetEntropyBitsTrm(binValue);
        return;
    }

    m_range -= 2;
    if (binValue)
    {
        m_low += m_range;
        m_low <<= 7;
        m_range = 2 << 7;
        m_bitsLeft += 7;
    }
    else if (m_range >= 256)
        return;
    else
    {
        m_low <<= 1;
        m_range <<= 1;
        m_bitsLeft++;
    }

    if (m_bitsLeft >= 0)
        writeOut();
}

/** Move bits from register into bitstream */
void Entropy::writeOut()
{
    uint32_t leadByte = m_low >> (13 + m_bitsLeft);
    uint32_t low_mask = (uint32_t)(~0) >> (11 + 8 - m_bitsLeft);

    m_bitsLeft -= 8;
    m_low &= low_mask;

    if (leadByte == 0xff)
        m_numBufferedBytes++;
    else
    {
        uint32_t numBufferedBytes = m_numBufferedBytes;
        if (numBufferedBytes > 0)
        {
            uint32_t carry = leadByte >> 8;
            uint32_t byteTowrite = m_bufferedByte + carry;
            m_bitIf->writeByte(byteTowrite);

            byteTowrite = (0xff + carry) & 0xff;
            while (numBufferedBytes > 1)
            {
                m_bitIf->writeByte(byteTowrite);
                numBufferedBytes--;
            }
        }
        m_numBufferedBytes = 1;
        m_bufferedByte = (uint8_t)leadByte;
    }
}

const uint32_t g_entropyBits[128] =
{
    // Corrected table, most notably for last state
    0x07b23, 0x085f9, 0x074a0, 0x08cbc, 0x06ee4, 0x09354, 0x067f4, 0x09c1b, 0x060b0, 0x0a62a, 0x05a9c, 0x0af5b, 0x0548d, 0x0b955, 0x04f56, 0x0c2a9,
    0x04a87, 0x0cbf7, 0x045d6, 0x0d5c3, 0x04144, 0x0e01b, 0x03d88, 0x0e937, 0x039e0, 0x0f2cd, 0x03663, 0x0fc9e, 0x03347, 0x10600, 0x03050, 0x10f95,
    0x02d4d, 0x11a02, 0x02ad3, 0x12333, 0x0286e, 0x12cad, 0x02604, 0x136df, 0x02425, 0x13f48, 0x021f4, 0x149c4, 0x0203e, 0x1527b, 0x01e4d, 0x15d00,
    0x01c99, 0x166de, 0x01b18, 0x17017, 0x019a5, 0x17988, 0x01841, 0x18327, 0x016df, 0x18d50, 0x015d9, 0x19547, 0x0147c, 0x1a083, 0x0138e, 0x1a8a3,
    0x01251, 0x1b418, 0x01166, 0x1bd27, 0x01068, 0x1c77b, 0x00f7f, 0x1d18e, 0x00eda, 0x1d91a, 0x00e19, 0x1e254, 0x00d4f, 0x1ec9a, 0x00c90, 0x1f6e0,
    0x00c01, 0x1fef8, 0x00b5f, 0x208b1, 0x00ab6, 0x21362, 0x00a15, 0x21e46, 0x00988, 0x2285d, 0x00934, 0x22ea8, 0x008a8, 0x239b2, 0x0081d, 0x24577,
    0x007c9, 0x24ce6, 0x00763, 0x25663, 0x00710, 0x25e8f, 0x006a0, 0x26a26, 0x00672, 0x26f23, 0x005e8, 0x27ef8, 0x005ba, 0x284b5, 0x0055e, 0x29057,
    0x0050c, 0x29bab, 0x004c1, 0x2a674, 0x004a7, 0x2aa5e, 0x0046f, 0x2b32f, 0x0041f, 0x2c0ad, 0x003e7, 0x2ca8d, 0x003ba, 0x2d323, 0x0010c, 0x3bfbb
};

// [8 24] --> [stateMPS BitCost], [stateLPS BitCost]
const uint32_t g_entropyStateBits[128] =
{
    // Corrected table, most notably for last state
    0x01007b23, 0x000085f9, 0x020074a0, 0x00008cbc, 0x03006ee4, 0x01009354, 0x040067f4, 0x02009c1b,
    0x050060b0, 0x0200a62a, 0x06005a9c, 0x0400af5b, 0x0700548d, 0x0400b955, 0x08004f56, 0x0500c2a9,
    0x09004a87, 0x0600cbf7, 0x0a0045d6, 0x0700d5c3, 0x0b004144, 0x0800e01b, 0x0c003d88, 0x0900e937,
    0x0d0039e0, 0x0900f2cd, 0x0e003663, 0x0b00fc9e, 0x0f003347, 0x0b010600, 0x10003050, 0x0c010f95,
    0x11002d4d, 0x0d011a02, 0x12002ad3, 0x0d012333, 0x1300286e, 0x0f012cad, 0x14002604, 0x0f0136df,
    0x15002425, 0x10013f48, 0x160021f4, 0x100149c4, 0x1700203e, 0x1201527b, 0x18001e4d, 0x12015d00,
    0x19001c99, 0x130166de, 0x1a001b18, 0x13017017, 0x1b0019a5, 0x15017988, 0x1c001841, 0x15018327,
    0x1d0016df, 0x16018d50, 0x1e0015d9, 0x16019547, 0x1f00147c, 0x1701a083, 0x2000138e, 0x1801a8a3,
    0x21001251, 0x1801b418, 0x22001166, 0x1901bd27, 0x23001068, 0x1a01c77b, 0x24000f7f, 0x1a01d18e,
    0x25000eda, 0x1b01d91a, 0x26000e19, 0x1b01e254, 0x27000d4f, 0x1c01ec9a, 0x28000c90, 0x1d01f6e0,
    0x29000c01, 0x1d01fef8, 0x2a000b5f, 0x1e0208b1, 0x2b000ab6, 0x1e021362, 0x2c000a15, 0x1e021e46,
    0x2d000988, 0x1f02285d, 0x2e000934, 0x20022ea8, 0x2f0008a8, 0x200239b2, 0x3000081d, 0x21024577,
    0x310007c9, 0x21024ce6, 0x32000763, 0x21025663, 0x33000710, 0x22025e8f, 0x340006a0, 0x22026a26,
    0x35000672, 0x23026f23, 0x360005e8, 0x23027ef8, 0x370005ba, 0x230284b5, 0x3800055e, 0x24029057,
    0x3900050c, 0x24029bab, 0x3a0004c1, 0x2402a674, 0x3b0004a7, 0x2502aa5e, 0x3c00046f, 0x2502b32f,
    0x3d00041f, 0x2502c0ad, 0x3e0003e7, 0x2602ca8d, 0x3e0003ba, 0x2602d323, 0x3f00010c, 0x3f03bfbb,
};

const uint8_t g_nextState[128][2] =
{
    { 2, 1 }, { 0, 3 }, { 4, 0 }, { 1, 5 }, { 6, 2 }, { 3, 7 }, { 8, 4 }, { 5, 9 },
    { 10, 4 }, { 5, 11 }, { 12, 8 }, { 9, 13 }, { 14, 8 }, { 9, 15 }, { 16, 10 }, { 11, 17 },
    { 18, 12 }, { 13, 19 }, { 20, 14 }, { 15, 21 }, { 22, 16 }, { 17, 23 }, { 24, 18 }, { 19, 25 },
    { 26, 18 }, { 19, 27 }, { 28, 22 }, { 23, 29 }, { 30, 22 }, { 23, 31 }, { 32, 24 }, { 25, 33 },
    { 34, 26 }, { 27, 35 }, { 36, 26 }, { 27, 37 }, { 38, 30 }, { 31, 39 }, { 40, 30 }, { 31, 41 },
    { 42, 32 }, { 33, 43 }, { 44, 32 }, { 33, 45 }, { 46, 36 }, { 37, 47 }, { 48, 36 }, { 37, 49 },
    { 50, 38 }, { 39, 51 }, { 52, 38 }, { 39, 53 }, { 54, 42 }, { 43, 55 }, { 56, 42 }, { 43, 57 },
    { 58, 44 }, { 45, 59 }, { 60, 44 }, { 45, 61 }, { 62, 46 }, { 47, 63 }, { 64, 48 }, { 49, 65 },
    { 66, 48 }, { 49, 67 }, { 68, 50 }, { 51, 69 }, { 70, 52 }, { 53, 71 }, { 72, 52 }, { 53, 73 },
    { 74, 54 }, { 55, 75 }, { 76, 54 }, { 55, 77 }, { 78, 56 }, { 57, 79 }, { 80, 58 }, { 59, 81 },
    { 82, 58 }, { 59, 83 }, { 84, 60 }, { 61, 85 }, { 86, 60 }, { 61, 87 }, { 88, 60 }, { 61, 89 },
    { 90, 62 }, { 63, 91 }, { 92, 64 }, { 65, 93 }, { 94, 64 }, { 65, 95 }, { 96, 66 }, { 67, 97 },
    { 98, 66 }, { 67, 99 }, { 100, 66 }, { 67, 101 }, { 102, 68 }, { 69, 103 }, { 104, 68 }, { 69, 105 },
    { 106, 70 }, { 71, 107 }, { 108, 70 }, { 71, 109 }, { 110, 70 }, { 71, 111 }, { 112, 72 }, { 73, 113 },
    { 114, 72 }, { 73, 115 }, { 116, 72 }, { 73, 117 }, { 118, 74 }, { 75, 119 }, { 120, 74 }, { 75, 121 },
    { 122, 74 }, { 75, 123 }, { 124, 76 }, { 77, 125 }, { 124, 76 }, { 77, 125 }, { 126, 126 }, { 127, 127 }
};

}
