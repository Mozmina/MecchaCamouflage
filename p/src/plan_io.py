from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .core import Color, PaintPlan, PaintSample, PlanConfig, PlanMetadata


def plan_to_dict(plan: PaintPlan) -> dict[str, Any]:
    return {
        "version": plan.metadata.version if plan.metadata else "0.1.0",
        "generator": plan.metadata.generator if plan.metadata else "meccha-camouflage",
        "comment": plan.metadata.comment if plan.metadata else "",
        "config": vars(plan.config),
        "front_samples": [_sample_to_dict(sample) for sample in plan.front_samples],
        "side_samples": [_sample_to_dict(sample) for sample in plan.side_samples],
        "back_samples": [_sample_to_dict(sample) for sample in plan.back_samples],
    }


def plan_from_dict(data: dict[str, Any]) -> PaintPlan:
    config = PlanConfig(**data["config"])
    front = [_dict_to_sample(item) for item in data.get("front_samples", [])]
    side = [_dict_to_sample(item) for item in data.get("side_samples", [])]
    back = [_dict_to_sample(item) for item in data.get("back_samples", [])]
    metadata = PlanMetadata(
        version=data.get("version", "0.1.0"),
        generator=data.get("generator", "meccha-camouflage"),
        comment=data.get("comment", ""),
    )
    return PaintPlan(config=config, front_samples=front, side_samples=side, back_samples=back, metadata=metadata)


def _sample_to_dict(sample: PaintSample) -> dict[str, Any]:
    return {
        "u": sample.u,
        "v": sample.v,
        "r": sample.color.r,
        "g": sample.color.g,
        "b": sample.color.b,
        "roughness": sample.color.roughness,
        "metallic": sample.color.metallic,
        "apply_mode": sample.color.apply_mode,
        "alpha": sample.color.alpha,
        "floor_like": sample.floor_like,
        "priority": sample.priority,
        "radius": sample.radius,
        "weight": sample.weight,
    }


def _dict_to_sample(item: dict[str, Any]) -> PaintSample:
    return PaintSample(
        u=float(item["u"]),
        v=float(item["v"]),
        color=Color(
            r=float(item["r"]),
            g=float(item["g"]),
            b=float(item["b"]),
            roughness=float(item["roughness"]),
            metallic=float(item["metallic"]),
            apply_mode=int(item.get("apply_mode", 1)),
            alpha=float(item.get("alpha", 1.0)),
        ),
        floor_like=bool(item.get("floor_like", False)),
        priority=int(item.get("priority", 0)),
        radius=float(item.get("radius", 2.0)),
        weight=float(item.get("weight", 1.0)),
    )


def write_plan_json(plan: PaintPlan, path: str | Path) -> Path:
    out_path = Path(path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as file:
        json.dump(plan_to_dict(plan), file, indent=2)
    return out_path


def read_plan_json(path: str | Path) -> PaintPlan:
    with Path(path).open("r", encoding="utf-8") as file:
        data = json.load(file)
    return plan_from_dict(data)
