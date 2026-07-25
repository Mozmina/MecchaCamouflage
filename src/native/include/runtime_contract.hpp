#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <tuple>
#include <vector>

namespace runtime_contract
{
    // Supported Shipping build: FProperty ArrayDim@0x30,
    // ElementSize@0x34, PropertyFlags@0x38.
    constexpr std::size_t FPropertyElementSizeOffset = 0x34;
    // Keep direct dispatch bounded so one scheduler tick cannot monopolize the
    // game thread. This is a CPU safety limit, not a network pacing setting.
    constexpr int NativeRecordedPaintMaxCallsPerTick = 6;
    // Permit at most a small, game-owned recorded-paint lead. Waiting for zero
    // serializes every stroke; an unbounded lead recreates the visible dotted
    // frontier on joining clients.
    constexpr int NativeRecordedPaintQueueTargetStrokes = 4;
    constexpr int FastLocalCadenceMs = 1;
    constexpr std::uint64_t LocalDispatchCpuBudgetUs = 4'000;

    // UE 5.6 packs Metallic, Roughness, and Emissive into one material-properties
    // render target (R/G/B).  Channel 7 updates that target atomically.  Splitting
    // a sample into channels 5 and 6 doubles the material-properties work and allowed the
    // separate local replication route to render a second visible pass.
    constexpr std::array<std::uint8_t, 1> ProductionMaterialPaintChannels{7};

    constexpr std::size_t production_material_stroke_count(std::size_t sample_count)
    {
        return sample_count * ProductionMaterialPaintChannels.size();
    }

    constexpr std::size_t production_material_sample_index(std::size_t stroke_index)
    {
        return stroke_index / ProductionMaterialPaintChannels.size();
    }

    // Direct paint delegates mesh-radius and subdivision interpretation to the
    // game. These sentinel values preserve that native behavior for anchored
    // strokes without carrying a second transport-specific contract.
    constexpr float GamePaintMeshAnchorWorldRadiusAuto = 0.0f;
    constexpr int GamePaintMeshAnchorSubdivisionLevelAuto = 0;
    constexpr float GamePaintMeshAnchorSubdivisionPixelSizeAuto = 0.0f;
    constexpr int GamePaintMeshAnchorTemplateResolutionAuto = 0;

    // UObject flags are checked against Shipping disassembly for the supported
    // build.  0x20000000 is intentionally not rejected: it is not an object
    // destruction flag and treating it as one spuriously blocks live objects.
    constexpr std::uint32_t RFClassDefaultObject = 0x00000010u;
    constexpr std::uint32_t RFBeginDestroyed = 0x00008000u;
    constexpr std::uint32_t RFFinishDestroyed = 0x00010000u;
    constexpr std::uint32_t RFMirroredGarbage = 0x40000000u;
    constexpr std::uint32_t ObjectRejectMask =
        RFClassDefaultObject | RFBeginDestroyed | RFFinishDestroyed | RFMirroredGarbage;
    constexpr std::uint32_t ClassRejectMask = RFBeginDestroyed | RFFinishDestroyed | RFMirroredGarbage;

    constexpr bool uobject_flags_usable(std::uint32_t object_flags, std::uint32_t class_flags)
    {
        return (object_flags & ObjectRejectMask) == 0 && (class_flags & ClassRejectMask) == 0;
    }

    constexpr int min_value(int left, int right)
    {
        return left < right ? left : right;
    }

    constexpr int max_value(int left, int right)
    {
        return left > right ? left : right;
    }

    constexpr int clamp_value(int value, int minimum, int maximum)
    {
        return min_value(max_value(value, minimum), maximum);
    }

    // EPaintChannel: 0..3 address one render target, All addresses four, and
    // AlbedoMetallicRoughness addresses three. UE 5.6's AMRE (7) uses the
    // one material-properties target. The game limit is expressed in
    // render-target writes, not paint-stroke calls.
    constexpr int paint_channel_write_cost(int target_channel)
    {
        return target_channel == 4 ? 4 : (target_channel == 5 ? 3 : 1);
    }

    constexpr bool local_dispatch_can_append(int processed_calls,
                                             int scheduled_writes,
                                             int next_write_cost,
                                             int max_calls,
                                             int max_render_target_writes)
    {
        if (processed_calls <= 0)
        {
            return true;
        }
        return processed_calls < max_value(1, max_calls) &&
               scheduled_writes + max_value(1, next_write_cost) <=
                   max_value(1, max_render_target_writes);
    }

    constexpr bool local_dispatch_cpu_budget_reached(int processed_calls,
                                                     std::uint64_t elapsed_us)
    {
        return processed_calls > 0 && elapsed_us >= LocalDispatchCpuBudgetUs;
    }

    constexpr int recurring_scheduler_delay_ms(int requested_delay_ms)
    {
        return max_value(1, requested_delay_ms);
    }

    struct SpatialScanlineKey
    {
        int row;
        double horizontal;
        std::size_t original_ordinal;
    };

    inline int spatial_scanline_row(double top_z, double point_z, double row_height)
    {
        if (!std::isfinite(top_z) || !std::isfinite(point_z) ||
            !std::isfinite(row_height) || row_height <= 0.000001)
        {
            return 0;
        }
        return static_cast<int>(std::floor(std::max(0.0, top_z - point_z) / row_height));
    }

    inline bool spatial_scanline_less(const SpatialScanlineKey& left,
                                      const SpatialScanlineKey& right)
    {
        if (left.row != right.row)
        {
            return left.row < right.row;
        }
        if (left.horizontal != right.horizontal)
        {
            return left.horizontal < right.horizontal;
        }
        return left.original_ordinal < right.original_ordinal;
    }

    enum class ReplayRegion
    {
        Front,
        Side,
        Back,
    };

    enum class ReplayRegionMode
    {
        Paint,
        Fill,
        Skip,
    };

    enum class ReplayPass
    {
        Fill,
        Paint,
        Complete,
    };

    struct ReplayPassWindow
    {
        ReplayPass pass;
        std::size_t begin;
        std::size_t end;
    };

    // Resolve the pass containing an offset in the effective replay stream.  The
    // planner stores exclusive boundaries, so an offset exactly at a boundary is
    // reported as the next pass.  Clamp malformed boundaries here so diagnostics
    // remain safe even when a runtime limit truncates the planned stream.
    constexpr ReplayPassWindow replay_pass_window(std::size_t offset,
                                                  std::size_t total,
                                                  std::size_t fill_end)
    {
        const std::size_t safe_fill_end = std::min(fill_end, total);
        const std::size_t safe_offset = std::min(offset, total);
        if (safe_offset >= total)
        {
            return {ReplayPass::Complete, total, total};
        }
        if (safe_offset < safe_fill_end)
        {
            return {ReplayPass::Fill, 0, safe_fill_end};
        }
        return {ReplayPass::Paint, safe_fill_end, total};
    }

    struct ReplayCandidate
    {
        std::size_t sample_index;
        ReplayRegion region;
        ReplayRegionMode mode;
        int uv_island;
        double u;
        double v;
        bool has_current_view_position;
        double current_view_vertical;
        double fallback_view_vertical;
        double horizontal;
        std::size_t original_ordinal;
    };

    struct ReplayEntry
    {
        std::size_t sample_index;
        ReplayPass pass;
        ReplayRegion region;
        SpatialScanlineKey spatial_key;
    };

    struct ReplayPlan
    {
        std::vector<ReplayEntry> entries{};
        std::size_t fill_end{0};
        std::size_t fill_count{0};
        std::size_t paint_count{0};
        std::size_t fill_candidates{0};
        std::size_t fill_deduplicated{0};
        std::size_t paint_candidates{0};
        std::size_t paint_deduplicated{0};
        bool current_view_projection_fallback_used{false};
        std::size_t current_view_projection_fallback_candidates{0};
    };

    struct NaturalOrderOptions
    {
        bool enabled{false};
        std::uint32_t seed{0};
    };

    // Groups entries into a fixed 6x6 zone grid (independent of brush size, so
    // a tiny brush does not explode the zone count) and visits zones with a
    // randomized nearest-neighbor walk: finish the strokes in one zone, then
    // move to a nearby unfinished zone, rather than a global full-width sweep.
    // Within a zone, entries are shuffled so strokes do not line up straight.
    // A fixed seed reproduces the exact same order, which keeps this testable.
    inline void shuffle_natural_order(std::vector<ReplayEntry>& pending, std::uint32_t seed)
    {
        if (pending.size() < 2)
        {
            return;
        }

        constexpr int ClusterRows = 6;
        constexpr int ClusterColumns = 6;

        int min_row = pending.front().spatial_key.row;
        int max_row = min_row;
        double min_horizontal = pending.front().spatial_key.horizontal;
        double max_horizontal = min_horizontal;
        for (const auto& entry : pending)
        {
            min_row = std::min(min_row, entry.spatial_key.row);
            max_row = std::max(max_row, entry.spatial_key.row);
            min_horizontal = std::min(min_horizontal, entry.spatial_key.horizontal);
            max_horizontal = std::max(max_horizontal, entry.spatial_key.horizontal);
        }
        const double row_span = std::max(1.0, static_cast<double>(max_row - min_row));
        const double horizontal_span = std::max(0.000001, max_horizontal - min_horizontal);

        const auto cluster_of = [&](const ReplayEntry& entry) {
            const double row_fraction = (entry.spatial_key.row - min_row) / row_span;
            const double horizontal_fraction =
                (entry.spatial_key.horizontal - min_horizontal) / horizontal_span;
            const int cluster_row = std::min(ClusterRows - 1, static_cast<int>(row_fraction * ClusterRows));
            const int cluster_column =
                std::min(ClusterColumns - 1, static_cast<int>(horizontal_fraction * ClusterColumns));
            return cluster_row * ClusterColumns + cluster_column;
        };

        std::vector<std::vector<std::size_t>> cluster_members(
            static_cast<std::size_t>(ClusterRows * ClusterColumns));
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            cluster_members[static_cast<std::size_t>(cluster_of(pending[index]))].push_back(index);
        }

        std::vector<int> occupied_clusters;
        for (int cluster = 0; cluster < ClusterRows * ClusterColumns; ++cluster)
        {
            if (!cluster_members[static_cast<std::size_t>(cluster)].empty())
            {
                occupied_clusters.push_back(cluster);
            }
        }

        std::mt19937 rng(seed);
        std::vector<bool> visited(occupied_clusters.size(), false);
        std::uniform_int_distribution<std::size_t> start_pick(0, occupied_clusters.size() - 1);
        std::size_t current = start_pick(rng);
        std::vector<int> visit_order;
        visit_order.reserve(occupied_clusters.size());

        for (std::size_t step = 0; step < occupied_clusters.size(); ++step)
        {
            visited[current] = true;
            visit_order.push_back(occupied_clusters[current]);
            const int current_row = occupied_clusters[current] / ClusterColumns;
            const int current_column = occupied_clusters[current] % ClusterColumns;
            int nearest_distance = std::numeric_limits<int>::max();
            std::vector<std::size_t> ties;
            for (std::size_t candidate = 0; candidate < occupied_clusters.size(); ++candidate)
            {
                if (visited[candidate])
                {
                    continue;
                }
                const int candidate_row = occupied_clusters[candidate] / ClusterColumns;
                const int candidate_column = occupied_clusters[candidate] % ClusterColumns;
                const int row_delta = candidate_row > current_row ? candidate_row - current_row
                                                                   : current_row - candidate_row;
                const int column_delta = candidate_column > current_column
                                              ? candidate_column - current_column
                                              : current_column - candidate_column;
                const int distance = row_delta + column_delta;
                if (distance < nearest_distance)
                {
                    nearest_distance = distance;
                    ties.clear();
                    ties.push_back(candidate);
                }
                else if (distance == nearest_distance)
                {
                    ties.push_back(candidate);
                }
            }
            if (!ties.empty())
            {
                std::uniform_int_distribution<std::size_t> tie_pick(0, ties.size() - 1);
                current = ties[tie_pick(rng)];
            }
        }

        std::vector<ReplayEntry> ordered;
        ordered.reserve(pending.size());
        for (int cluster : visit_order)
        {
            auto& members = cluster_members[static_cast<std::size_t>(cluster)];
            std::shuffle(members.begin(), members.end(), rng);
            for (std::size_t member_index : members)
            {
                ordered.push_back(pending[member_index]);
            }
        }
        pending = std::move(ordered);
    }

    // A sample's real captured paint color, indexed the same way as ReplayEntry::sample_index.
    struct ReplaySampleColor
    {
        double r{0.0};
        double g{0.0};
        double b{0.0};
    };

    struct ColorBatchOptions
    {
        bool enabled{false};
        // A cluster stops splitting once its widest R/G/B channel range falls at or
        // below this threshold (0..1 scale). Smaller values -> more, tighter clusters
        // (fine per-color detail); larger values -> fewer, broader clusters (coarse
        // color blocking).
        double split_threshold{0.12};
        int max_clusters{8};
        std::uint32_t seed{0};
    };

    // Groups the Paint-pass portion of `entries` (the range from fill_end to the end)
    // into color batches using a deterministic median-cut style split: repeatedly
    // divide whichever cluster has the widest single-channel (R, G, or B) color range,
    // until every cluster is tight enough or the cluster cap is reached. This makes the
    // number of batches adapt automatically to how colorful the surface actually is: a
    // near-flat surface stays as one or two batches, a multi-color design splits into
    // more. Clusters are then visited largest-first (the dominant color masses before
    // the smaller accents, like a painter blocking in the big shapes first), and within
    // each cluster shuffle_natural_order still runs so a single-color batch is not
    // painted in a straight line either. The Fill portion of `entries` is left untouched.
    inline void reorder_paint_entries_by_color_batch(
        std::vector<ReplayEntry>& entries,
        std::size_t fill_end,
        const std::vector<ReplaySampleColor>& sample_colors,
        const ColorBatchOptions& options)
    {
        if (!options.enabled || fill_end >= entries.size())
        {
            return;
        }

        std::vector<std::vector<std::size_t>> clusters;
        clusters.emplace_back();
        for (std::size_t i = fill_end; i < entries.size(); ++i)
        {
            clusters[0].push_back(i);
        }

        const auto sample_color_for = [&](std::size_t entry_index) -> const ReplaySampleColor& {
            static const ReplaySampleColor fallback{};
            const auto sample_index = entries[entry_index].sample_index;
            return sample_index < sample_colors.size() ? sample_colors[sample_index] : fallback;
        };

        const auto channel_range = [&](const std::vector<std::size_t>& members, int& best_channel) -> double {
            if (members.size() < 2)
            {
                best_channel = 0;
                return 0.0;
            }
            double min_r = 1.0, max_r = 0.0, min_g = 1.0, max_g = 0.0, min_b = 1.0, max_b = 0.0;
            for (std::size_t member : members)
            {
                const auto& color = sample_color_for(member);
                min_r = std::min(min_r, color.r);
                max_r = std::max(max_r, color.r);
                min_g = std::min(min_g, color.g);
                max_g = std::max(max_g, color.g);
                min_b = std::min(min_b, color.b);
                max_b = std::max(max_b, color.b);
            }
            const double range_r = max_r - min_r;
            const double range_g = max_g - min_g;
            const double range_b = max_b - min_b;
            const double best = std::max({range_r, range_g, range_b});
            best_channel = (best <= range_r) ? 0 : ((best <= range_g) ? 1 : 2);
            return best;
        };

        const int cluster_cap = std::max(1, options.max_clusters);
        while (static_cast<int>(clusters.size()) < cluster_cap)
        {
            std::size_t split_index = clusters.size();
            double split_range = 0.0;
            int split_channel = 0;
            for (std::size_t i = 0; i < clusters.size(); ++i)
            {
                if (clusters[i].size() < 2)
                {
                    continue;
                }
                int channel = 0;
                const double range = channel_range(clusters[i], channel);
                if (range > split_range)
                {
                    split_range = range;
                    split_index = i;
                    split_channel = channel;
                }
            }
            if (split_index >= clusters.size() || split_range <= options.split_threshold)
            {
                break;
            }
            auto& members = clusters[split_index];
            std::sort(members.begin(), members.end(), [&](std::size_t left, std::size_t right) {
                const auto& lc = sample_color_for(left);
                const auto& rc = sample_color_for(right);
                const double lv = split_channel == 0 ? lc.r : (split_channel == 1 ? lc.g : lc.b);
                const double rv = split_channel == 0 ? rc.r : (split_channel == 1 ? rc.g : rc.b);
                return lv < rv;
            });
            const std::size_t midpoint = members.size() / 2;
            std::vector<std::size_t> second_half(members.begin() + static_cast<std::ptrdiff_t>(midpoint),
                                                 members.end());
            members.erase(members.begin() + static_cast<std::ptrdiff_t>(midpoint), members.end());
            clusters.push_back(std::move(second_half));
        }

        std::stable_sort(clusters.begin(), clusters.end(), [](const auto& left, const auto& right) {
            return left.size() > right.size();
        });

        std::vector<ReplayEntry> ordered;
        ordered.reserve(entries.size() - fill_end);
        std::uint32_t cluster_seed = options.seed;
        for (auto& members : clusters)
        {
            std::vector<ReplayEntry> cluster_entries;
            cluster_entries.reserve(members.size());
            for (std::size_t member : members)
            {
                cluster_entries.push_back(entries[member]);
            }
            shuffle_natural_order(cluster_entries, cluster_seed);
            cluster_seed = cluster_seed * 1000003u + 0x2545F491u;
            for (auto& entry : cluster_entries)
            {
                ordered.push_back(entry);
            }
        }

        for (std::size_t i = fill_end; i < entries.size(); ++i)
        {
            entries[i] = ordered[i - fill_end];
        }
    }

    inline ReplayPlan build_single_brush_replay_plan(
        const std::vector<ReplayCandidate>& candidates,
        int texture_size,
        double brush_size_texels,
        double fill_radius_texels,
        const NaturalOrderOptions& natural_order = NaturalOrderOptions{})
    {
        ReplayPlan plan{};
        const double texture_size_double = static_cast<double>(max_value(1, texture_size));
        const double fill_cell_uv = fill_radius_texels * 0.75 / texture_size_double;
        bool fill_all_regions = false;
        for (const auto& candidate : candidates)
        {
            if (candidate.mode == ReplayRegionMode::Fill)
            {
                fill_all_regions = true;
                break;
            }
        }
        double vertical_top = 0.0;
        double vertical_bottom = 0.0;
        bool have_vertical_bounds = false;
        const auto selected_vertical = [](const ReplayCandidate& candidate) {
            return candidate.has_current_view_position && std::isfinite(candidate.current_view_vertical)
                       ? candidate.current_view_vertical
                       : (std::isfinite(candidate.fallback_view_vertical)
                              ? candidate.fallback_view_vertical
                              : 0.0);
        };
        for (const auto& candidate : candidates)
        {
            if (!fill_all_regions && candidate.mode == ReplayRegionMode::Skip)
            {
                continue;
            }
            const double vertical = selected_vertical(candidate);
            if (!have_vertical_bounds)
            {
                vertical_top = vertical;
                vertical_bottom = vertical;
                have_vertical_bounds = true;
            }
            else
            {
                vertical_top = std::max(vertical_top, vertical);
                vertical_bottom = std::min(vertical_bottom, vertical);
            }
            if (!candidate.has_current_view_position || !std::isfinite(candidate.current_view_vertical))
            {
                ++plan.current_view_projection_fallback_candidates;
            }
        }
        plan.current_view_projection_fallback_used =
            plan.current_view_projection_fallback_candidates > 0;
        const double vertical_span = std::max(0.001, vertical_top - vertical_bottom);
        auto append_pass = [&](ReplayPass pass,
                               ReplayRegionMode required_mode,
                               double dedupe_cell_uv,
                               double row_size_texels,
                               bool include_all_regions = false) {
            std::set<std::tuple<int, int, int, int>> emitted_cells{};
            std::vector<ReplayEntry> pending{};
            const double row_height = std::max(
                0.000001,
                vertical_span * std::max(0.001, row_size_texels) / texture_size_double);
            for (const auto& candidate : candidates)
            {
                if (!include_all_regions && candidate.mode != required_mode)
                {
                    continue;
                }
                if (pass == ReplayPass::Fill)
                {
                    ++plan.fill_candidates;
                }
                else if (pass == ReplayPass::Paint)
                {
                    ++plan.paint_candidates;
                }
                if (dedupe_cell_uv > 0.000001)
                {
                    const auto cell_coordinate = [&](double value) {
                        const double finite_value = std::isfinite(value) ? value : 0.0;
                        return static_cast<int>(std::floor(
                            std::max(0.0, std::min(1.0, finite_value)) / dedupe_cell_uv));
                    };
                    const auto cell = std::make_tuple(
                        static_cast<int>(candidate.region),
                        candidate.uv_island,
                        cell_coordinate(candidate.u),
                        cell_coordinate(candidate.v));
                    if (!emitted_cells.insert(cell).second)
                    {
                        if (pass == ReplayPass::Fill)
                        {
                            ++plan.fill_deduplicated;
                        }
                        else if (pass == ReplayPass::Paint)
                        {
                            ++plan.paint_deduplicated;
                        }
                        continue;
                    }
                }
                pending.push_back(
                    {candidate.sample_index,
                     pass,
                     candidate.region,
                     {spatial_scanline_row(vertical_top,
                                           selected_vertical(candidate),
                                           row_height),
                      candidate.horizontal,
                      candidate.original_ordinal}});
            }
            if (natural_order.enabled)
            {
                const std::uint32_t pass_seed =
                    natural_order.seed ^ (pass == ReplayPass::Fill ? 0x9E3779B9u : 0x85EBCA6Bu);
                shuffle_natural_order(pending, pass_seed);
            }
            else
            {
                std::stable_sort(pending.begin(), pending.end(), [](const auto& left, const auto& right) {
                    return spatial_scanline_less(left.spatial_key, right.spatial_key);
                });
            }
            plan.entries.insert(plan.entries.end(), pending.begin(), pending.end());
        };

        append_pass(ReplayPass::Fill,
                    ReplayRegionMode::Fill,
                    fill_cell_uv,
                    fill_radius_texels,
                    fill_all_regions);
        plan.fill_end = plan.entries.size();
        plan.fill_count = plan.fill_end;
        append_pass(ReplayPass::Paint,
                    ReplayRegionMode::Paint,
                    0.0,
                    brush_size_texels);
        plan.paint_count = plan.entries.size() - plan.fill_end;
        return plan;
    }

    // Compression is intentionally plan-local: a widened direct stroke may
    // cover only samples that share its region, UV island, and final material
    // payload.  Fill entries are never compressed.
    struct AdaptivePaintSample
    {
        double u;
        double v;
        ReplayRegion region;
        int uv_island;
        double r;
        double g;
        double b;
        bool paint_eligible;
        bool safe;
        std::uint64_t material_key;
    };

    struct AdaptiveReplayEntry
    {
        ReplayEntry replay;
        double radius_multiplier{1.0};
    };

    struct AdaptivePaintPlan
    {
        std::vector<AdaptiveReplayEntry> entries{};
        std::size_t compressed_paint_entries{0};
        std::size_t expanded_paint_entries{0};
    };

    inline AdaptivePaintPlan build_adaptive_paint_plan(
        const std::vector<ReplayEntry>& replay_entries,
        const std::vector<AdaptivePaintSample>& samples,
        double base_radius_uv,
        double tolerance_percent,
        double edge_margin_uv = 0.0)
    {
        AdaptivePaintPlan plan{};
        plan.entries.reserve(replay_entries.size());
        if (replay_entries.empty())
        {
            return plan;
        }
        if (tolerance_percent <= 0.0 || base_radius_uv <= 0.000001 || samples.empty())
        {
            for (const auto& entry : replay_entries)
            {
                plan.entries.push_back({entry, 1.0});
            }
            return plan;
        }

        int grid_size = 128;
        if (samples.size() > 200000)
        {
            grid_size = 256;
        }
        if (samples.size() > 500000)
        {
            grid_size = 512;
        }
        std::vector<std::vector<std::size_t>> grid(
            static_cast<std::size_t>(grid_size * grid_size));
        const auto cell_coordinate = [&](double value) {
            const double finite_value = std::isfinite(value) ? value : 0.0;
            return std::clamp(static_cast<int>(std::floor(finite_value * grid_size)), 0, grid_size - 1);
        };
        for (std::size_t index = 0; index < samples.size(); ++index)
        {
            const auto& sample = samples[index];
            grid[static_cast<std::size_t>(cell_coordinate(sample.v) * grid_size +
                                          cell_coordinate(sample.u))]
                .push_back(index);
        }

        const double threshold = std::clamp(tolerance_percent, 0.0, 10.0) / 100.0 * std::sqrt(3.0);
        const double threshold_squared = threshold * threshold;
        const auto same_payload = [](const AdaptivePaintSample& center,
                                     const AdaptivePaintSample& other) {
            return center.paint_eligible && center.safe && other.paint_eligible && other.safe &&
                   center.region == other.region && center.uv_island == other.uv_island &&
                   center.material_key == other.material_key;
        };
        const auto color_distance_squared = [](const AdaptivePaintSample& left,
                                               const AdaptivePaintSample& right) {
            const double dr = left.r - right.r;
            const double dg = left.g - right.g;
            const double db = left.b - right.b;
            return dr * dr + dg * dg + db * db;
        };
        const auto visit_nearby = [&](const AdaptivePaintSample& center,
                                      double radius_uv,
                                      const auto& visit) {
            const double safe_radius = std::max(0.0, radius_uv);
            const double radius_squared = safe_radius * safe_radius;
            const int min_u = cell_coordinate(center.u - safe_radius);
            const int max_u = cell_coordinate(center.u + safe_radius);
            const int min_v = cell_coordinate(center.v - safe_radius);
            const int max_v = cell_coordinate(center.v + safe_radius);
            for (int cell_v = min_v; cell_v <= max_v; ++cell_v)
            {
                for (int cell_u = min_u; cell_u <= max_u; ++cell_u)
                {
                    for (const auto other_index : grid[static_cast<std::size_t>(cell_v * grid_size + cell_u)])
                    {
                        const auto& other = samples[other_index];
                        const double du = other.u - center.u;
                        const double dv = other.v - center.v;
                        if (du * du + dv * dv <= radius_squared)
                        {
                            visit(other_index, other);
                        }
                    }
                }
            }
        };

        std::vector<bool> covered(samples.size(), false);
        std::vector<AdaptiveReplayEntry> paint_entries{};
        paint_entries.reserve(replay_entries.size());
        constexpr std::array<double, 4> multipliers{4.0, 3.0, 2.0, 1.5};
        for (const auto& entry : replay_entries)
        {
            if (entry.pass != ReplayPass::Paint || entry.sample_index >= samples.size())
            {
                plan.entries.push_back({entry, 1.0});
                continue;
            }
            if (covered[entry.sample_index])
            {
                ++plan.compressed_paint_entries;
                continue;
            }

            const auto& center = samples[entry.sample_index];
            double multiplier = 1.0;
            if (center.paint_eligible && center.safe)
            {
                for (const auto candidate_multiplier : multipliers)
                {
                    const double check_radius = std::max(
                        0.0, candidate_multiplier * base_radius_uv - std::max(0.0, edge_margin_uv));
                    bool valid = true;
                    visit_nearby(center, check_radius, [&](std::size_t, const AdaptivePaintSample& other) {
                        if (!same_payload(center, other) ||
                            color_distance_squared(center, other) > threshold_squared)
                        {
                            valid = false;
                        }
                    });
                    if (valid)
                    {
                        multiplier = candidate_multiplier;
                        break;
                    }
                }
            }

            paint_entries.push_back({entry, multiplier});
            if (multiplier > 1.0)
            {
                ++plan.expanded_paint_entries;
            }
            covered[entry.sample_index] = true;
            const double coverage_radius = std::max(
                0.0, multiplier * base_radius_uv - std::max(0.0, edge_margin_uv));
            visit_nearby(center, coverage_radius, [&](std::size_t other_index,
                                                       const AdaptivePaintSample& other) {
                if (same_payload(center, other) &&
                    color_distance_squared(center, other) <= threshold_squared)
                {
                    covered[other_index] = true;
                }
            });
        }
        std::stable_sort(paint_entries.begin(), paint_entries.end(), [](const auto& left, const auto& right) {
            return left.radius_multiplier > right.radius_multiplier;
        });
        plan.entries.insert(plan.entries.end(), paint_entries.begin(), paint_entries.end());
        return plan;
    }

    constexpr bool event_watch_generation_active(bool enabled,
                                                 std::uint64_t current_generation,
                                                 std::uint64_t captured_generation)
    {
        return enabled && current_generation == captured_generation;
    }

}
