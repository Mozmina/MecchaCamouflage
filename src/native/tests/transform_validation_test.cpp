#include "../include/sdk.hpp"
#include "../include/runtime_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

int main()
{
    sdk::FTransform valid{};
    valid.Rotation = {0.0, 0.0, 0.705717, 0.708494};
    valid.Translation = {-295.483835, 6223.716973, 8.323874};
    valid.Scale3D = {1.1, 1.1, 1.1};

    sdk::FTransform malformed = valid;
    malformed.Rotation = {0.0, 0.0, -2889820.0, 11160673.0};

    if (!sdk::transform_is_plausible(valid))
    {
        return 1;
    }
    if (sdk::transform_is_plausible(malformed))
    {
        return 2;
    }

    std::array<std::uint8_t, 0x48> fake_property{};
    const std::int32_t array_dim = 1;
    const std::int32_t element_size = 0x20;
    const std::uint64_t property_flags = 0x0018001000000000ULL;
    std::memcpy(fake_property.data() + 0x30, &array_dim, sizeof(array_dim));
    std::memcpy(fake_property.data() + runtime_contract::FPropertyElementSizeOffset,
                &element_size,
                sizeof(element_size));
    std::memcpy(fake_property.data() + 0x38, &property_flags, sizeof(property_flags));
    std::int32_t decoded_element_size = 0;
    std::memcpy(&decoded_element_size,
                fake_property.data() + runtime_contract::FPropertyElementSizeOffset,
                sizeof(decoded_element_size));
    if (decoded_element_size != element_size || runtime_contract::FPropertyElementSizeOffset != 0x34)
    {
        return 3;
    }

    if (!runtime_contract::requires_internal_no_resend(false, false, false, false) ||
        runtime_contract::requires_internal_no_resend(true, false, false, false) ||
        runtime_contract::requires_internal_no_resend(false, true, false, false) ||
        runtime_contract::requires_internal_no_resend(false, false, true, false) ||
        runtime_contract::requires_internal_no_resend(false, false, false, true) ||
        runtime_contract::InternalNoResendMaxCallsPerTick != 6)
    {
        return 4;
    }

    if (!runtime_contract::uobject_flags_usable(0, 0) ||
        runtime_contract::uobject_flags_usable(runtime_contract::RFClassDefaultObject, 0) ||
        runtime_contract::uobject_flags_usable(runtime_contract::RFBeginDestroyed, 0) ||
        runtime_contract::uobject_flags_usable(runtime_contract::RFFinishDestroyed, 0) ||
        runtime_contract::uobject_flags_usable(runtime_contract::RFMirroredGarbage, 0) ||
        runtime_contract::uobject_flags_usable(0, runtime_contract::RFBeginDestroyed) ||
        runtime_contract::uobject_flags_usable(0, runtime_contract::RFFinishDestroyed) ||
        runtime_contract::uobject_flags_usable(0, runtime_contract::RFMirroredGarbage) ||
        !runtime_contract::uobject_flags_usable(0x20000000u, 0))
    {
        return 5;
    }

    const auto fastest = runtime_contract::resolve_pacing(
        20,
        50,
        20,
        20,
        24,
        6);
    if (fastest.remote_batch_limit != 20 || fastest.remote_delay_ms != 50 ||
        fastest.local_batch_limit != 6 || fastest.local_delay_ms != 17)
    {
        return 6;
    }

    const auto tuned = runtime_contract::resolve_pacing(
        7,
        125,
        20,
        20,
        24,
        6);
    if (tuned.remote_batch_limit != 7 || tuned.remote_delay_ms != 125 ||
        tuned.local_batch_limit != 6 || tuned.local_delay_ms != 17)
    {
        return 7;
    }

    const auto clamped = runtime_contract::resolve_pacing(0, 0, 20, 20, 24, 6);
    if (clamped.remote_batch_limit != 1 || clamped.remote_delay_ms != 50 ||
        clamped.local_batch_limit != 6 || clamped.local_delay_ms != 17)
    {
        return 8;
    }

    if (!runtime_contract::event_watch_generation_active(true, 7, 7) ||
        runtime_contract::event_watch_generation_active(false, 7, 7) ||
        runtime_contract::event_watch_generation_active(true, 8, 7))
    {
        return 9;
    }

    if (runtime_contract::paint_channel_write_cost(4) != 4 ||
        runtime_contract::paint_channel_write_cost(5) != 3 ||
        runtime_contract::paint_channel_write_cost(0) != 1 ||
        !runtime_contract::local_dispatch_can_append(0, 0, 4, 6, 6) ||
        runtime_contract::local_dispatch_can_append(1, 4, 4, 6, 6) ||
        !runtime_contract::local_dispatch_cpu_budget_reached(1, 4'000) ||
        runtime_contract::local_dispatch_cpu_budget_reached(0, 10'000) ||
        runtime_contract::recurring_scheduler_delay_ms(0) != 1)
    {
        return 10;
    }

    std::array<runtime_contract::SpatialScanlineKey, 4> scanline{{
        {runtime_contract::spatial_scanline_row(100.0, 100.0, 10.0), 10.0, 0},
        {runtime_contract::spatial_scanline_row(100.0, 90.0, 10.0), -20.0, 1},
        {runtime_contract::spatial_scanline_row(100.0, 100.0, 10.0), -10.0, 2},
        {runtime_contract::spatial_scanline_row(100.0, 100.0, 10.0), -10.0, 3},
    }};
    std::stable_sort(scanline.begin(), scanline.end(), runtime_contract::spatial_scanline_less);
    if (scanline[0].original_ordinal != 2 || scanline[1].original_ordinal != 3 ||
        scanline[2].original_ordinal != 0 || scanline[3].original_ordinal != 1)
    {
        return 11;
    }

    const std::vector<runtime_contract::TwoBrushReplayCandidate> routed_candidates{
        {0, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Fill,
         0, 0.10, 0.10, true, 100.0, 10.0, -5.0, 0},
        {1, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         1, 0.20, 0.20, true, 90.0, 9.0, 0.0, 1},
        {2, runtime_contract::ReplayRegion::Front, runtime_contract::ReplayRegionMode::Skip,
         2, 0.30, 0.30, true, 80.0, 8.0, 5.0, 2},
    };
    const auto routed_plan = runtime_contract::build_two_brush_replay_plan(
        routed_candidates,
        1024,
        20.0,
        10.0,
        80.0);
    if (routed_plan.entries.size() != 3 ||
        routed_plan.fill_end != 1 || routed_plan.coarse_end != 2 ||
        routed_plan.fill_count != 1 || routed_plan.coarse_paint_count != 1 ||
        routed_plan.fine_paint_count != 1 ||
        routed_plan.entries[0].pass != runtime_contract::ReplayPass::Fill ||
        routed_plan.entries[0].sample_index != 0 ||
        routed_plan.entries[1].pass != runtime_contract::ReplayPass::CoarsePaint ||
        routed_plan.entries[1].sample_index != 1 ||
        routed_plan.entries[2].pass != runtime_contract::ReplayPass::FinePaint ||
        routed_plan.entries[2].sample_index != 1)
    {
        return 12;
    }

    const std::vector<runtime_contract::TwoBrushReplayCandidate> dedupe_candidates{
        {0, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Fill,
         0, 0.100, 0.100, true, 100.0, 10.0, -5.0, 0},
        {1, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Fill,
         0, 0.105, 0.105, true, 99.0, 9.0, -4.0, 1},
        {2, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         1, 0.200, 0.200, true, 90.0, 8.0, -3.0, 2},
        {3, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         1, 0.205, 0.205, true, 89.0, 7.0, -2.0, 3},
        {4, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         1, 0.250, 0.250, true, 80.0, 6.0, -1.0, 4},
    };
    const auto dedupe_plan = runtime_contract::build_two_brush_replay_plan(
        dedupe_candidates,
        1024,
        20.0,
        10.0,
        80.0);
    if (dedupe_plan.entries.size() != 6 ||
        dedupe_plan.fill_end != 1 || dedupe_plan.coarse_end != 3 ||
        dedupe_plan.fill_count != 1 || dedupe_plan.coarse_paint_count != 2 ||
        dedupe_plan.fine_paint_count != 3 ||
        dedupe_plan.fill_candidates != 2 || dedupe_plan.fill_deduplicated != 1 ||
        dedupe_plan.coarse_paint_candidates != 3 || dedupe_plan.coarse_paint_deduplicated != 1 ||
        dedupe_plan.entries[0].sample_index != 0 ||
        dedupe_plan.entries[1].sample_index != 2 ||
        dedupe_plan.entries[2].sample_index != 4)
    {
        return 13;
    }

    const std::vector<runtime_contract::TwoBrushReplayCandidate> reference_order_candidates{
        {0, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 90.0, 1000.0, 10.0, 0},
        {1, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.20, 0.20, true, 100.0, 0.0, 10.0, 1},
        {2, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.30, 0.30, true, 100.0, 0.0, -10.0, 2},
        {3, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.40, 0.40, false, 999.0, 80.0, 0.0, 3},
    };
    const auto reference_order_plan = runtime_contract::build_two_brush_replay_plan(
        reference_order_candidates,
        1024,
        20.0,
        10.0,
        80.0);
    const std::array<std::size_t, 4> expected_reference_order{{2, 1, 0, 3}};
    for (std::size_t index = 0; index < expected_reference_order.size(); ++index)
    {
        if (reference_order_plan.entries[index].sample_index != expected_reference_order[index] ||
            reference_order_plan.entries[reference_order_plan.coarse_end + index].sample_index != expected_reference_order[index])
        {
            return 14;
        }
    }
    if (!reference_order_plan.reference_position_fallback_used ||
        reference_order_plan.reference_position_fallback_candidates != 1)
    {
        return 14;
    }

    const std::vector<runtime_contract::TwoBrushReplayCandidate> region_order_candidates{
        {0, runtime_contract::ReplayRegion::Front, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 100.0, 0.0, 0.0, 0},
        {1, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 100.0, 0.0, 0.0, 1},
        {2, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 100.0, 0.0, 0.0, 2},
    };
    const auto region_order_plan = runtime_contract::build_two_brush_replay_plan(
        region_order_candidates,
        1024,
        20.0,
        10.0,
        80.0);
    const std::array<std::size_t, 3> expected_region_order{{2, 1, 0}};
    for (std::size_t index = 0; index < expected_region_order.size(); ++index)
    {
        if (region_order_plan.entries[index].sample_index != expected_region_order[index] ||
            region_order_plan.entries[region_order_plan.coarse_end + index].sample_index != expected_region_order[index])
        {
            return 15;
        }
    }

    const auto supported_brush_pipeline =
        runtime_contract::resolve_brush_pipeline_version(2, false, false);
    const auto missing_brush_pipeline =
        runtime_contract::resolve_brush_pipeline_version(0, false, false);
    const auto legacy_brush_pipeline =
        runtime_contract::resolve_brush_pipeline_version(1, false, false);
    const auto future_brush_pipeline =
        runtime_contract::resolve_brush_pipeline_version(3, false, false);
    const auto fractional_brush_pipeline =
        runtime_contract::resolve_brush_pipeline_version(2.5, false, false);
    const auto preview_without_brush_pipeline =
        runtime_contract::resolve_brush_pipeline_version(0, true, false);
    if (!supported_brush_pipeline.required || !supported_brush_pipeline.supported ||
        missing_brush_pipeline.supported || legacy_brush_pipeline.supported ||
        future_brush_pipeline.supported || fractional_brush_pipeline.supported ||
        preview_without_brush_pipeline.required ||
        !preview_without_brush_pipeline.supported)
    {
        return 16;
    }

    if (!runtime_contract::packed_manager_precommit_matches(0x1000, 0x1000) ||
        runtime_contract::packed_manager_precommit_matches(0x1000, 0x2000) ||
        runtime_contract::packed_manager_precommit_matches(0x1000, 0) ||
        runtime_contract::packed_manager_precommit_matches(0, 0))
    {
        return 17;
    }

    if (!runtime_contract::paired_paint_cancel_safe_to_observe(false, 20, 0) ||
        !runtime_contract::paired_paint_cancel_safe_to_observe(true, 20, 20) ||
        runtime_contract::paired_paint_cancel_safe_to_observe(true, 20, 0) ||
        runtime_contract::paired_paint_cancel_safe_to_observe(true, 40, 20))
    {
        return 18;
    }

    if (runtime_contract::PackedMeshAnchorWorldRadiusAuto != 0.0f ||
        runtime_contract::PackedMeshAnchorCoverageSafetyFactor != 0.91 ||
        runtime_contract::PackedMeshAnchorExpectedRadiusCalibration != 3.5 ||
        !runtime_contract::packed_mesh_anchor_requests_world_radius_conversion(
            runtime_contract::PackedMeshAnchorWorldRadiusAuto) ||
        runtime_contract::packed_mesh_anchor_requests_world_radius_conversion(20.0f / 1024.0f) ||
        !runtime_contract::packed_mesh_anchor_world_radius_contract_valid(0.0f, 10.0f / 1024.0f) ||
        !runtime_contract::packed_mesh_anchor_world_radius_contract_valid(1.665f, 10.0f / 1024.0f) ||
        runtime_contract::packed_mesh_anchor_world_radius_contract_valid(10.0f / 1024.0f,
                                                                         10.0f / 1024.0f) ||
        runtime_contract::packed_mesh_anchor_world_radius_contract_valid(
            std::numeric_limits<float>::quiet_NaN(),
            10.0f / 1024.0f))
    {
        return 19;
    }

    const float source_wire_test_radius = 10.0f / 1024.0f;
    float resolved_wire_radius = -1.0f;
    if (!runtime_contract::resolve_packed_wire_brush_radius(source_wire_test_radius,
                                                            1.0,
                                                            resolved_wire_radius) ||
        resolved_wire_radius != source_wire_test_radius ||
        !runtime_contract::resolve_packed_wire_brush_radius(source_wire_test_radius,
                                                            3.5,
                                                            resolved_wire_radius) ||
        resolved_wire_radius != static_cast<float>(
                                    static_cast<double>(source_wire_test_radius) * 3.5) ||
        source_wire_test_radius != 10.0f / 1024.0f ||
        runtime_contract::resolve_packed_wire_brush_radius(0.0f, 1.0, resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(-0.1f, 1.0, resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(
            std::numeric_limits<float>::quiet_NaN(), 1.0, resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(
            std::numeric_limits<float>::infinity(), 1.0, resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(source_wire_test_radius,
                                                            0.0,
                                                            resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(
            source_wire_test_radius,
            std::numeric_limits<double>::quiet_NaN(),
            resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(
            source_wire_test_radius,
            std::numeric_limits<double>::infinity(),
            resolved_wire_radius) ||
        runtime_contract::resolve_packed_wire_brush_radius(0.5f, 3.0, resolved_wire_radius))
    {
        return 20;
    }

    constexpr auto auto_subdivision_tail =
        runtime_contract::packed_mesh_anchor_auto_subdivision_tail();
    if (runtime_contract::PackedMeshAnchorSubdivisionLevelAuto != 0 ||
        runtime_contract::PackedMeshAnchorSubdivisionPixelSizeAuto != 0.0f ||
        runtime_contract::PackedMeshAnchorTemplateResolutionAuto != 0 ||
        auto_subdivision_tail.size() != 4 ||
        auto_subdivision_tail[0] != 0 ||
        auto_subdivision_tail[1] != 0 ||
        auto_subdivision_tail[2] != 0 ||
        auto_subdivision_tail[3] != 0 ||
        !runtime_contract::packed_mesh_anchor_requests_native_subdivision_preflight(
            runtime_contract::PackedMeshAnchorSubdivisionLevelAuto,
            runtime_contract::PackedMeshAnchorSubdivisionPixelSizeAuto,
            runtime_contract::PackedMeshAnchorTemplateResolutionAuto) ||
        runtime_contract::packed_mesh_anchor_requests_native_subdivision_preflight(20, 0.0f, 0) ||
        runtime_contract::packed_mesh_anchor_requests_native_subdivision_preflight(0, 2.0f, 0) ||
        runtime_contract::packed_mesh_anchor_requests_native_subdivision_preflight(0, 0.0f, 1024))
    {
        return 21;
    }

    const auto fill_window = runtime_contract::replay_pass_window(0, 100, 20, 80);
    const auto coarse_window = runtime_contract::replay_pass_window(20, 100, 20, 80);
    const auto fine_window = runtime_contract::replay_pass_window(80, 100, 20, 80);
    const auto complete_window = runtime_contract::replay_pass_window(100, 100, 20, 80);
    const auto clamped_window = runtime_contract::replay_pass_window(999, 10, 50, 2);
    if (fill_window.pass != runtime_contract::ReplayPass::Fill ||
        fill_window.begin != 0 || fill_window.end != 20 ||
        coarse_window.pass != runtime_contract::ReplayPass::CoarsePaint ||
        coarse_window.begin != 20 || coarse_window.end != 80 ||
        fine_window.pass != runtime_contract::ReplayPass::FinePaint ||
        fine_window.begin != 80 || fine_window.end != 100 ||
        complete_window.pass != runtime_contract::ReplayPass::Complete ||
        complete_window.begin != 100 || complete_window.end != 100 ||
        clamped_window.pass != runtime_contract::ReplayPass::Complete ||
        clamped_window.begin != 10 || clamped_window.end != 10)
    {
        return 22;
    }

    if (runtime_contract::receiver_queue_rendered_strokes(5596, 4119, 0) != 1477 ||
        runtime_contract::receiver_queue_rendered_strokes(5596, 4200, 1477) != 1477 ||
        runtime_contract::receiver_queue_rendered_strokes(5596, 0, 1477) != 5596 ||
        runtime_contract::receiver_queue_rendered_strokes(5596, 7420, 0) != 0 ||
        runtime_contract::receiver_queue_drain_complete(1, 2) ||
        runtime_contract::receiver_queue_drain_complete(0, 1) ||
        !runtime_contract::receiver_queue_drain_complete(0, 2) ||
        runtime_contract::receiver_queue_idle_threshold_reached(0, 120000, 120000) ||
        runtime_contract::receiver_queue_idle_threshold_reached(1, 119999, 120000) ||
        !runtime_contract::receiver_queue_idle_threshold_reached(1, 120000, 120000) ||
        !runtime_contract::receiver_queue_idle_threshold_reached(1, 120001, 120000))
    {
        return 23;
    }
    return 0;
}
