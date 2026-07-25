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

    if (!runtime_contract::event_watch_generation_active(true, 7, 7) ||
        runtime_contract::event_watch_generation_active(false, 7, 7) ||
        runtime_contract::event_watch_generation_active(true, 8, 7))
    {
        return 9;
    }

    if (runtime_contract::paint_channel_write_cost(4) != 4 ||
        runtime_contract::paint_channel_write_cost(5) != 3 ||
        runtime_contract::paint_channel_write_cost(7) != 1 ||
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

    const std::vector<runtime_contract::ReplayCandidate> routed_candidates{
        {0, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Fill,
         0, 0.10, 0.10, true, 100.0, 10.0, -5.0, 0},
        {1, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         1, 0.20, 0.20, true, 90.0, 9.0, 0.0, 1},
        {2, runtime_contract::ReplayRegion::Front, runtime_contract::ReplayRegionMode::Skip,
         2, 0.30, 0.30, true, 80.0, 8.0, 5.0, 2},
    };
    const auto routed_plan = runtime_contract::build_single_brush_replay_plan(
        routed_candidates, 1024, 5.0, 80.0);
    if (routed_plan.entries.size() != 4 ||
        routed_plan.fill_end != 3 ||
        routed_plan.fill_count != 3 || routed_plan.paint_count != 1 ||
        routed_plan.fill_candidates != 3 || routed_plan.fill_deduplicated != 0 ||
        routed_plan.entries[0].pass != runtime_contract::ReplayPass::Fill ||
        routed_plan.entries[0].sample_index != 0 ||
        routed_plan.entries[1].pass != runtime_contract::ReplayPass::Fill ||
        routed_plan.entries[1].sample_index != 1 ||
        routed_plan.entries[2].pass != runtime_contract::ReplayPass::Fill ||
        routed_plan.entries[2].sample_index != 2 ||
        routed_plan.entries[3].pass != runtime_contract::ReplayPass::Paint ||
        routed_plan.entries[3].sample_index != 1)
    {
        return 12;
    }

    const std::vector<runtime_contract::ReplayCandidate> dedupe_candidates{
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
    const auto dedupe_plan = runtime_contract::build_single_brush_replay_plan(
        dedupe_candidates, 1024, 5.0, 80.0);
    if (dedupe_plan.entries.size() != 6 ||
        dedupe_plan.fill_end != 3 ||
        dedupe_plan.fill_count != 3 || dedupe_plan.paint_count != 3 ||
        dedupe_plan.fill_candidates != 5 || dedupe_plan.fill_deduplicated != 2 ||
        dedupe_plan.paint_candidates != 3 || dedupe_plan.paint_deduplicated != 0 ||
        dedupe_plan.entries[0].sample_index != 0 ||
        dedupe_plan.entries[1].sample_index != 2 ||
        dedupe_plan.entries[2].sample_index != 4)
    {
        return 13;
    }

    const std::vector<runtime_contract::ReplayCandidate> current_view_order_candidates{
        {0, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 90.0, 1000.0, 10.0, 0},
        {1, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.20, 0.20, true, 100.0, 0.0, 10.0, 1},
        {2, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.30, 0.30, true, 100.0, 0.0, -10.0, 2},
        {3, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.40, 0.40, false, 999.0, 80.0, 0.0, 3},
    };
    const auto current_view_order_plan = runtime_contract::build_single_brush_replay_plan(
        current_view_order_candidates, 1024, 5.0, 80.0);
    const std::array<std::size_t, 4> expected_current_view_order{{2, 1, 0, 3}};
    for (std::size_t index = 0; index < expected_current_view_order.size(); ++index)
    {
        if (current_view_order_plan.entries[index].sample_index != expected_current_view_order[index])
        {
            return 14;
        }
    }
    if (!current_view_order_plan.current_view_projection_fallback_used ||
        current_view_order_plan.current_view_projection_fallback_candidates != 1)
    {
        return 14;
    }

    const std::vector<runtime_contract::ReplayCandidate> cross_region_view_order_candidates{
        {0, runtime_contract::ReplayRegion::Front, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 300.0, 0.0, 0.0, 0},
        {1, runtime_contract::ReplayRegion::Side, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 200.0, 0.0, 0.0, 1},
        {2, runtime_contract::ReplayRegion::Back, runtime_contract::ReplayRegionMode::Paint,
         0, 0.10, 0.10, true, 100.0, 0.0, 0.0, 2},
    };
    const auto cross_region_view_order_plan = runtime_contract::build_single_brush_replay_plan(
        cross_region_view_order_candidates, 1024, 5.0, 80.0);
    const std::array<std::size_t, 3> expected_cross_region_view_order{{0, 1, 2}};
    for (std::size_t index = 0; index < expected_cross_region_view_order.size(); ++index)
    {
        if (cross_region_view_order_plan.entries[index].sample_index != expected_cross_region_view_order[index])
        {
            return 15;
        }
    }

    const auto fill_window = runtime_contract::replay_pass_window(0, 100, 20);
    const auto paint_window = runtime_contract::replay_pass_window(20, 100, 20);
    const auto complete_window = runtime_contract::replay_pass_window(100, 100, 20);
    const auto clamped_window = runtime_contract::replay_pass_window(999, 10, 50);
    if (fill_window.pass != runtime_contract::ReplayPass::Fill ||
        fill_window.begin != 0 || fill_window.end != 20 ||
        paint_window.pass != runtime_contract::ReplayPass::Paint ||
        paint_window.begin != 20 || paint_window.end != 100 ||
        complete_window.pass != runtime_contract::ReplayPass::Complete ||
        complete_window.begin != 100 || complete_window.end != 100 ||
        clamped_window.pass != runtime_contract::ReplayPass::Complete ||
        clamped_window.begin != 10 || clamped_window.end != 10)
    {
        return 22;
    }

    const std::vector<runtime_contract::AdaptivePaintSample> adaptive_samples{
        {0.10, 0.10, runtime_contract::ReplayRegion::Front, 0, 0.50, 0.50, 0.50, true, true, 1},
        {0.102, 0.10, runtime_contract::ReplayRegion::Front, 0, 0.50, 0.50, 0.50, true, true, 1},
        {0.30, 0.10, runtime_contract::ReplayRegion::Front, 0, 0.20, 0.20, 0.20, true, true, 1},
        {0.60, 0.10, runtime_contract::ReplayRegion::Front, 1, 0.50, 0.50, 0.50, true, true, 1},
    };
    const std::vector<runtime_contract::ReplayEntry> adaptive_entries{
        {0, runtime_contract::ReplayPass::Paint, runtime_contract::ReplayRegion::Front, {0, 0.0, 0}},
        {1, runtime_contract::ReplayPass::Paint, runtime_contract::ReplayRegion::Front, {0, 1.0, 1}},
        {2, runtime_contract::ReplayPass::Paint, runtime_contract::ReplayRegion::Front, {0, 2.0, 2}},
        {3, runtime_contract::ReplayPass::Paint, runtime_contract::ReplayRegion::Front, {0, 3.0, 3}},
    };
    const auto no_compression = runtime_contract::build_adaptive_paint_plan(
        adaptive_entries, adaptive_samples, 0.01, 0.0);
    if (no_compression.entries.size() != adaptive_entries.size() ||
        no_compression.compressed_paint_entries != 0 ||
        no_compression.entries[0].radius_multiplier != 1.0 ||
        no_compression.entries[1].replay.sample_index != 1)
    {
        return 24;
    }
    const auto compressed = runtime_contract::build_adaptive_paint_plan(
        adaptive_entries, adaptive_samples, 0.01, 1.0);
    if (compressed.entries.size() != 3 ||
        compressed.compressed_paint_entries != 1 ||
        compressed.entries[0].replay.sample_index != 0 ||
        compressed.entries[0].radius_multiplier != 4.0 ||
        compressed.entries[1].replay.sample_index != 2 ||
        compressed.entries[2].replay.sample_index != 3)
    {
        return 25;
    }

    if (runtime_contract::production_material_stroke_count(3) != 3 ||
        runtime_contract::production_material_sample_index(0) != 0 ||
        runtime_contract::production_material_sample_index(1) != 1 ||
        runtime_contract::production_material_sample_index(2) != 2)
    {
        return 23;
    }
    if (runtime_contract::ProductionMaterialPaintChannels !=
        std::array<std::uint8_t, 1>{
            static_cast<std::uint8_t>(sdk::EPaintChannel::AlbedoMetallicRoughnessEmissive)})
    {
        return 26;
    }

    // Natural stroke order (F6/F7 "natural paint"): opt-in only. The default
    // (omitted) parameter must keep producing the exact scanline order the
    // asserts above depend on, while an explicit NaturalOrderOptions{true, seed}
    // must build zones progressively (not a straight left-right raster) and stay
    // reproducible for a fixed seed.
    std::vector<runtime_contract::ReplayCandidate> natural_order_candidates{};
    natural_order_candidates.reserve(24);
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 6; ++column)
        {
            const std::size_t index = natural_order_candidates.size();
            const double vertical = 100.0 - (row * 20.0);
            const double horizontal = -50.0 + (column * 20.0);
            natural_order_candidates.push_back(
                {index, runtime_contract::ReplayRegion::Front, runtime_contract::ReplayRegionMode::Paint,
                 static_cast<int>(index), 0.01 * (index + 1), 0.01 * (index + 1),
                 true, vertical, vertical, horizontal, index});
        }
    }

    const auto scanline_plan = runtime_contract::build_single_brush_replay_plan(
        natural_order_candidates, 1024, 5.0, 80.0);
    if (scanline_plan.entries.size() != natural_order_candidates.size() ||
        scanline_plan.paint_count != natural_order_candidates.size() ||
        scanline_plan.fill_count != 0)
    {
        return 30;
    }

    const auto natural_plan_a = runtime_contract::build_single_brush_replay_plan(
        natural_order_candidates, 1024, 5.0, 80.0,
        runtime_contract::NaturalOrderOptions{true, 12345u});
    if (natural_plan_a.entries.size() != natural_order_candidates.size() ||
        natural_plan_a.paint_count != natural_order_candidates.size() ||
        natural_plan_a.fill_count != 0)
    {
        return 31;
    }

    std::array<bool, 24> seen{};
    for (const auto& entry : natural_plan_a.entries)
    {
        if (entry.sample_index >= seen.size() || seen[entry.sample_index])
        {
            return 32;
        }
        seen[entry.sample_index] = true;
    }
    if (std::any_of(seen.begin(), seen.end(), [](bool value) { return !value; }))
    {
        return 32;
    }

    bool natural_order_differs_from_scanline = false;
    for (std::size_t index = 0; index < natural_order_candidates.size(); ++index)
    {
        if (scanline_plan.entries[index].sample_index != natural_plan_a.entries[index].sample_index)
        {
            natural_order_differs_from_scanline = true;
            break;
        }
    }
    if (!natural_order_differs_from_scanline)
    {
        return 33;
    }

    const auto natural_plan_a_repeat = runtime_contract::build_single_brush_replay_plan(
        natural_order_candidates, 1024, 5.0, 80.0,
        runtime_contract::NaturalOrderOptions{true, 12345u});
    for (std::size_t index = 0; index < natural_order_candidates.size(); ++index)
    {
        if (natural_plan_a.entries[index].sample_index != natural_plan_a_repeat.entries[index].sample_index)
        {
            return 34;
        }
    }

    const auto natural_plan_b = runtime_contract::build_single_brush_replay_plan(
        natural_order_candidates, 1024, 5.0, 80.0,
        runtime_contract::NaturalOrderOptions{true, 999u});
    bool different_seed_differs = false;
    for (std::size_t index = 0; index < natural_order_candidates.size(); ++index)
    {
        if (natural_plan_a.entries[index].sample_index != natural_plan_b.entries[index].sample_index)
        {
            different_seed_differs = true;
            break;
        }
    }
    if (!different_seed_differs)
    {
        return 35;
    }

    // Natural paint color batching (stage 2/3): strokes must be regrouped so every
    // stroke of one color batch is painted before the next batch starts, like a
    // painter blocking in one color mass at a time, instead of a scanline sweep that
    // interleaves colors as it crosses the surface. The Paint-only candidates are run
    // through the real planner first (so entries carry real spatial keys); two synthetic
    // Fill entries are then prepended by hand to confirm the Fill portion is left
    // completely untouched. (Candidates are built Paint-only here — mixing in Fill-mode
    // candidates would pull every candidate into the base Fill pass via `fill_all_regions`,
    // which is unrelated existing behavior this test does not want to exercise.)
    std::vector<runtime_contract::ReplayCandidate> color_batch_candidates{};
    color_batch_candidates.reserve(12);
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            const std::size_t index = color_batch_candidates.size();
            const double vertical = 100.0 - (row * 20.0);
            const double horizontal = -50.0 + (column * 30.0);
            color_batch_candidates.push_back(
                {index, runtime_contract::ReplayRegion::Front, runtime_contract::ReplayRegionMode::Paint,
                 static_cast<int>(index), 0.01 * (index + 1), 0.01 * (index + 1),
                 true, vertical, vertical, horizontal, index});
        }
    }

    const auto color_batch_paint_plan = runtime_contract::build_single_brush_replay_plan(
        color_batch_candidates, 1024, 5.0, 80.0);
    if (color_batch_paint_plan.entries.size() != color_batch_candidates.size() ||
        color_batch_paint_plan.fill_end != 0 ||
        color_batch_paint_plan.paint_count != 12)
    {
        return 40;
    }

    std::vector<runtime_contract::ReplaySampleColor> color_batch_sample_colors(color_batch_candidates.size());
    for (std::size_t index = 0; index < color_batch_candidates.size(); ++index)
    {
        color_batch_sample_colors[index] = (index % 2 == 0)
            ? runtime_contract::ReplaySampleColor{0.9, 0.05, 0.05}
            : runtime_contract::ReplaySampleColor{0.05, 0.05, 0.9};
    }

    constexpr std::size_t color_batch_fill_end = 2;
    const std::vector<runtime_contract::ReplayEntry> color_batch_fill_entries{
        {100, runtime_contract::ReplayPass::Fill, runtime_contract::ReplayRegion::Back, {0, 0.0, 0}},
        {101, runtime_contract::ReplayPass::Fill, runtime_contract::ReplayRegion::Back, {0, 1.0, 1}},
    };

    auto color_batch_entries = color_batch_fill_entries;
    color_batch_entries.insert(color_batch_entries.end(),
                               color_batch_paint_plan.entries.begin(),
                               color_batch_paint_plan.entries.end());
    if (color_batch_entries.size() != color_batch_fill_end + color_batch_candidates.size())
    {
        return 40;
    }

    runtime_contract::reorder_paint_entries_by_color_batch(
        color_batch_entries,
        color_batch_fill_end,
        color_batch_sample_colors,
        runtime_contract::ColorBatchOptions{true, 0.3, 8, 555u});

    for (std::size_t index = 0; index < color_batch_fill_end; ++index)
    {
        if (color_batch_entries[index].sample_index != color_batch_fill_entries[index].sample_index)
        {
            return 41;
        }
    }

    std::array<bool, 12> color_batch_seen{};
    for (std::size_t index = color_batch_fill_end; index < color_batch_entries.size(); ++index)
    {
        const auto sample_index = color_batch_entries[index].sample_index;
        if (sample_index >= color_batch_seen.size() || color_batch_seen[sample_index])
        {
            return 42;
        }
        color_batch_seen[sample_index] = true;
    }
    if (std::any_of(color_batch_seen.begin(), color_batch_seen.end(), [](bool value) { return !value; }))
    {
        return 42;
    }

    int color_batch_switch_count = 0;
    bool have_previous_color_batch = false;
    bool previous_is_red = false;
    for (std::size_t index = color_batch_fill_end; index < color_batch_entries.size(); ++index)
    {
        const auto sample_index = color_batch_entries[index].sample_index;
        const bool is_red = (sample_index % 2 == 0);
        if (have_previous_color_batch && is_red != previous_is_red)
        {
            ++color_batch_switch_count;
        }
        previous_is_red = is_red;
        have_previous_color_batch = true;
    }
    if (color_batch_switch_count != 1)
    {
        return 43;
    }

    auto color_batch_entries_repeat = color_batch_fill_entries;
    color_batch_entries_repeat.insert(color_batch_entries_repeat.end(),
                                      color_batch_paint_plan.entries.begin(),
                                      color_batch_paint_plan.entries.end());
    runtime_contract::reorder_paint_entries_by_color_batch(
        color_batch_entries_repeat,
        color_batch_fill_end,
        color_batch_sample_colors,
        runtime_contract::ColorBatchOptions{true, 0.3, 8, 555u});
    for (std::size_t index = 0; index < color_batch_entries.size(); ++index)
    {
        if (color_batch_entries[index].sample_index != color_batch_entries_repeat[index].sample_index)
        {
            return 44;
        }
    }

    return 0;
}
