from __future__ import annotations

import math
import random
from typing import Callable

from .models import Color, PaintPlan, PaintSample, PlanConfig


def build_default_config() -> PlanConfig:
    return PlanConfig()


def _grid_layout(sample_count: int, ratio: float) -> tuple[int, int]:
    if sample_count <= 0:
        return 1, 1
    cols = max(1, int(math.sqrt(sample_count * ratio)))
    rows = max(1, int(math.ceil(sample_count / cols)))
    return cols, rows


def _stratified_uv_grid(
    count: int,
    width: int,
    height: int,
    seed: int,
    jitter: float,
) -> list[tuple[float, float]]:
    rng = random.Random(seed)
    ratio = max(1.0, width / max(1, height))
    cols, rows = _grid_layout(count, ratio)
    samples: list[tuple[float, float]] = []
    target = count
    for y in range(rows):
        for x in range(cols):
            if len(samples) >= target:
                break
            base_x = (x + 0.5) / cols
            base_y = (y + 0.5) / rows
            jx = (rng.random() - 0.5) * (1.0 / cols) * jitter
            jy = (rng.random() - 0.5) * (1.0 / rows) * jitter
            u = min(0.9999, max(0.0001, base_x + jx))
            v = min(0.9999, max(0.0001, base_y + jy))
            samples.append((u, v))
    while len(samples) < target:
        nx = rng.random()
        ny = rng.random()
        samples.append((nx, ny))
    return samples


def _estimate_color_from_position(
    u: float,
    v: float,
    source: Callable[[float, float], Color],
) -> Color:
    return source(u, v)


def simulate_paint_distribution(plan: PaintPlan) -> tuple[dict[str, float], dict[str, int]]:
    buckets = {"front": 0, "side": 0, "back": 0, "total": 0}
    counts = {"front": len(plan.front_samples), "side": len(plan.side_samples), "back": len(plan.back_samples)}
    buckets["front"] = counts["front"]
    buckets["side"] = counts["side"]
    buckets["back"] = counts["back"]
    buckets["total"] = counts["front"] + counts["side"] + counts["back"]
    return buckets, counts


def estimate_readback_cost_ms(sample_count: int, texture_area: int) -> float:
    if sample_count <= 0 or texture_area <= 0:
        return 0.0
    base_ms = 0.12 * math.log2(max(2, sample_count))
    texture_factor = max(1.0, math.log2(max(1, texture_area) / 4096.0))
    return base_ms * texture_factor


def compose_plan(
    config: PlanConfig,
    color_sampler: Callable[[float, float], Color],
) -> PaintPlan:
    config = PlanConfig(
        viewport_width=max(1, config.viewport_width),
        viewport_height=max(1, config.viewport_height),
        texture_width=max(1, config.texture_width),
        texture_height=max(1, config.texture_height),
        sample_count=max(1, config.sample_count),
        min_front_hits=max(1, config.min_front_hits),
        target_front_hits=max(1, config.target_front_hits),
        preferred_front_hits=max(1, config.preferred_front_hits),
        random_seed=config.random_seed,
        jitter=clamp01(config.jitter),
        include_side=config.include_side,
        include_back=config.include_back,
        include_front_material_channels=config.include_front_material_channels,
        full_body_metallic_only=config.full_body_metallic_only,
    )

    desired = config.sample_count
    front_ratio = 1.0
    side_ratio = 0.0
    back_ratio = 0.0

    if config.include_side and config.include_back:
        front_ratio = 0.65
        side_ratio = 0.2
        back_ratio = 0.15
    elif config.include_side:
        front_ratio = 0.8
        side_ratio = 0.2
        back_ratio = 0.0
    elif config.include_back:
        front_ratio = 0.8
        side_ratio = 0.0
        back_ratio = 0.2

    front_count = min(desired, max(config.min_front_hits, min(desired, int(desired * front_ratio))))
    side_count = int((desired - front_count) * (side_ratio / max(1e-12, side_ratio + back_ratio)))
    if side_count < 0:
        side_count = 0
    back_count = max(0, desired - front_count - side_count)

    front_samples = _build_samples(
        count=front_count,
        count_limit=front_count,
        color_sampler=color_sampler,
        seed=config.random_seed + 101,
        jitter=config.jitter,
        config=config,
        priority=1,
        floor_like_threshold=0.25,
        radius=2.6,
        weight_scale=1.0,
        width=config.viewport_width,
        height=config.viewport_height,
    )
    side_samples = _build_samples(
        count=side_count,
        count_limit=side_count if side_count > 0 else 0,
        color_sampler=color_sampler,
        seed=config.random_seed + 202,
        jitter=config.jitter * 0.75,
        config=config,
        priority=2,
        floor_like_threshold=0.0,
        radius=3.0,
        weight_scale=0.7,
        width=config.viewport_width,
        height=config.viewport_height,
    )
    back_samples = _build_samples(
        count=back_count,
        count_limit=back_count if back_count > 0 else 0,
        color_sampler=color_sampler,
        seed=config.random_seed + 303,
        jitter=config.jitter * 0.75,
        config=config,
        priority=3,
        floor_like_threshold=0.0,
        radius=3.2,
        weight_scale=0.7,
        width=config.viewport_width,
        height=config.viewport_height,
    )

    return PaintPlan(
        config=config,
        front_samples=front_samples,
        side_samples=side_samples,
        back_samples=back_samples,
        metadata=None,
    )


def _build_samples(
    count: int,
    count_limit: int,
    color_sampler: Callable[[float, float], Color],
    seed: int,
    jitter: float,
    config: PlanConfig,
    priority: int,
    floor_like_threshold: float,
    radius: float,
    weight_scale: float,
    width: int,
    height: int,
) -> list[PaintSample]:
    if count <= 0 or count_limit <= 0:
        return []

    points = _stratified_uv_grid(min(count, count_limit), width, height, seed, jitter)
    samples: list[PaintSample] = []
    for u, v in points[:count]:
        color = _estimate_color_from_position(u, v, color_sampler)
        samples.append(
            PaintSample(
                u=u,
                v=v,
                color=color,
                floor_like=v > (1.0 - floor_like_threshold),
                priority=priority,
                radius=radius,
                weight=weight_scale,
            )
        )
    return samples


def clamp01(value: float) -> float:
    return max(0.0, min(1.0, value))


def build_reference_color_from_grid(
    x: int,
    y: int,
    width: int,
    height: int,
    palette: tuple[Color, Color, Color],
) -> Color:
    region = (x * 3 // max(1, width), y * 3 // max(1, height))
    if region == (0, 0):
        c = palette[0]
    elif region == (1, 0):
        c = palette[1]
    else:
        c = palette[2]
    return Color(
        r=c.r,
        g=c.g,
        b=c.b,
        roughness=c.roughness,
        metallic=c.metallic,
        apply_mode=c.apply_mode,
        alpha=c.alpha,
    )


def build_color_sampler_from_args(
    viewport: tuple[int, int],
    image_path: str | None,
    fallback_color: tuple[float, float, float],
    fallback_roughness: float,
    fallback_metallic: float,
) -> Callable[[float, float], Color]:
    if image_path:
        try:
            from PIL import Image
        except Exception as exc:
            raise RuntimeError(
                "Pillow is required for --input-image support. Install with "
                "`python -m pip install pillow`."
            ) from exc

        with Image.open(image_path) as image:
            source = image.convert("RGB")
            width, height = source.size
            pixels = source.load()

            def sample_from_image(u: float, v: float) -> Color:
                x = min(width - 1, max(0, int(u * width)))
                y = min(height - 1, max(0, int((1.0 - v) * height)))
                r, g, b = pixels[x, y]
                return Color(
                    r=r / 255.0,
                    g=g / 255.0,
                    b=b / 255.0,
                    roughness=fallback_roughness,
                    metallic=fallback_metallic,
                    apply_mode=1,
                    alpha=1.0,
                )

            return sample_from_image

    base_width, base_height = max(1, viewport[0]), max(1, viewport[1])
    fallback = Color(
        r=fallback_color[0],
        g=fallback_color[1],
        b=fallback_color[2],
        roughness=fallback_roughness,
        metallic=fallback_metallic,
        apply_mode=1,
        alpha=1.0,
    )

    def sample_from_grid(u: float, v: float) -> Color:
        # simple stable checker-like fallback using coordinate hash.
        blend_x = int(u * 2.0) % 2
        blend_y = int(v * 2.0) % 2
        if (blend_x + blend_y) % 2 == 0:
            return fallback
        return Color(
            r=min(1.0, fallback.r + 0.15),
            g=min(1.0, fallback.g + 0.10),
            b=min(1.0, fallback.b + 0.05),
            roughness=fallback.roughness,
            metallic=fallback.metallic,
            apply_mode=1,
            alpha=1.0,
        )

    _ = base_width, base_height
    return sample_from_grid
