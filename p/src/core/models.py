from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional


def clamp_unit(value: float) -> float:
    return max(0.0, min(1.0, value))


@dataclass(frozen=True)
class Color:
    r: float
    g: float
    b: float
    roughness: float = 0.65
    metallic: float = 0.0
    apply_mode: int = 1
    alpha: float = 1.0

    def to_linear(self) -> "Color":
        return Color(
            r=srgb_to_linear(self.r),
            g=srgb_to_linear(self.g),
            b=srgb_to_linear(self.b),
            roughness=clamp_unit(self.roughness),
            metallic=clamp_unit(self.metallic),
            apply_mode=self.apply_mode,
            alpha=clamp_unit(self.alpha),
        )

    def to_srgb(self) -> "Color":
        return Color(
            r=linear_to_srgb(self.r),
            g=linear_to_srgb(self.g),
            b=linear_to_srgb(self.b),
            roughness=clamp_unit(self.roughness),
            metallic=clamp_unit(self.metallic),
            apply_mode=self.apply_mode,
            alpha=clamp_unit(self.alpha),
        )

    def as_tuple(self) -> tuple[float, float, float, float, float, int, float]:
        return (
            clamp_unit(self.r),
            clamp_unit(self.g),
            clamp_unit(self.b),
            clamp_unit(self.roughness),
            clamp_unit(self.metallic),
            int(self.apply_mode),
            clamp_unit(self.alpha),
        )


@dataclass(frozen=True)
class PaintSample:
    u: float
    v: float
    color: Color
    floor_like: bool = False
    priority: int = 0
    radius: float = 2.0
    weight: float = 1.0

    def clamp(self) -> "PaintSample":
        return PaintSample(
            u=clamp_unit(self.u),
            v=clamp_unit(self.v),
            color=Color(
                r=clamp_unit(self.color.r),
                g=clamp_unit(self.color.g),
                b=clamp_unit(self.color.b),
                roughness=clamp_unit(self.color.roughness),
                metallic=clamp_unit(self.color.metallic),
                apply_mode=1,
                alpha=clamp_unit(self.color.alpha),
            ),
            floor_like=self.floor_like,
            priority=int(self.priority),
            radius=max(0.0, self.radius),
            weight=max(0.0, self.weight),
        )


@dataclass(frozen=True)
class PlanConfig:
    viewport_width: int = 1920
    viewport_height: int = 1080
    texture_width: int = 2048
    texture_height: int = 2048
    sample_count: int = 2048
    min_front_hits: int = 256
    target_front_hits: int = 768
    preferred_front_hits: int = 1536
    random_seed: int = 1234
    jitter: float = 0.5
    include_side: bool = False
    include_back: bool = False
    include_front_material_channels: bool = True
    full_body_metallic_only: bool = False


@dataclass
class PlanMetadata:
    version: str = "0.1.0"
    generator: str = "meccha-camouflage"
    comment: str = ""


@dataclass
class PaintPlan:
    config: PlanConfig
    front_samples: List[PaintSample]
    side_samples: List[PaintSample]
    back_samples: List[PaintSample]
    metadata: Optional[PlanMetadata] = None

    def __post_init__(self) -> None:
        self.front_samples = [sample.clamp() for sample in self.front_samples]
        self.side_samples = [sample.clamp() for sample in self.side_samples]
        self.back_samples = [sample.clamp() for sample in self.back_samples]

    @property
    def total_samples(self) -> int:
        return len(self.front_samples) + len(self.side_samples) + len(self.back_samples)


def srgb_to_linear(v: float) -> float:
    v = clamp_unit(v)
    if v <= 0.04045:
        return v / 12.92
    return ((v + 0.055) / 1.055) ** 2.4


def linear_to_srgb(v: float) -> float:
    v = clamp_unit(v)
    if v <= 0.0031308:
        return v * 12.92
    return 1.055 * (v ** (1.0 / 2.4)) - 0.055
