#!/usr/bin/env python3
"""Empirical bridge from QEMU M7 work units to measured device cycles.

This intentionally does not call QEMU cycle-accurate. Coefficients are fitted
from DWT measurements bound to one device profile; predictions disclose their
held-out/sample residual instead of presenting an invented absolute fps.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


class TimingError(RuntimeError):
    pass


def load(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TimingError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise TimingError(f"JSON root must be an object: {path}")
    return value


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump(value: dict[str, Any], path: Path | None) -> None:
    text = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if path:
        path.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


def validate_profile(profile: dict[str, Any]) -> None:
    if profile.get("schema_version") != 1:
        raise TimingError("unsupported device profile")
    if not profile.get("build", {}).get("map_sha256"):
        raise TimingError("device profile is not bound to a linker map")
    if not profile.get("clock_profiles"):
        raise TimingError("device profile has no measured clock")


def fit(calibration: dict[str, Any], profile_path: Path) -> dict[str, Any]:
    profile = load(profile_path)
    validate_profile(profile)
    samples = calibration.get("samples")
    if not isinstance(samples, list) or not samples:
        raise TimingError("calibration needs at least one sample")

    components: set[str] | None = None
    for sample in samples:
        qemu = sample.get("qemu_units", {})
        device = sample.get("device_cycles", {})
        if not isinstance(qemu, dict) or not isinstance(device, dict):
            raise TimingError("each sample needs qemu_units and device_cycles objects")
        common = set(qemu) & set(device)
        components = common if components is None else components & common
    if not components:
        raise TimingError("samples have no common measured components")

    coefficients: dict[str, float] = {}
    for component in sorted(components):
        units = sum(float(sample["qemu_units"][component]) for sample in samples)
        cycles = sum(float(sample["device_cycles"][component]) for sample in samples)
        if units <= 0 or cycles < 0:
            raise TimingError(f"invalid calibration totals for {component}")
        coefficients[component] = cycles / units

    fixed_values = [float(sample.get("fixed_cycles", 0)) for sample in samples]
    fixed_mean = sum(fixed_values) / len(fixed_values)
    residuals: list[float] = []
    sample_results: list[dict[str, Any]] = []
    for sample, fixed in zip(samples, fixed_values):
        actual = fixed + sum(float(sample["device_cycles"][name]) for name in components)
        predicted = fixed_mean + sum(
            float(sample["qemu_units"][name]) * coefficients[name] for name in components
        )
        relative = abs(predicted - actual) / actual if actual else 0.0
        residuals.append(relative)
        sample_results.append(
            {
                "name": sample.get("name", "unnamed"),
                "actual_cycles": actual,
                "predicted_cycles": predicted,
                "relative_error": relative,
            }
        )

    return {
        "schema_version": 1,
        "kind": "empirical-qemu-to-device-cycle-model",
        "device_profile_sha256": digest(profile_path),
        "device_map_sha256": profile["build"]["map_sha256"],
        "coefficients_cycles_per_qemu_unit": coefficients,
        "fixed_cycles_mean": fixed_mean,
        "validation": {
            "sample_count": len(samples),
            "status": "measured-multi-sample" if len(samples) >= 2 else "single-sample-no-error-bound",
            "mean_relative_error": sum(residuals) / len(residuals),
            "max_relative_error": max(residuals),
            "samples": sample_results,
        },
        "claim": "empirical timing estimate; QEMU itself remains instruction-count-only",
    }


def predict(model: dict[str, Any], workload: dict[str, Any], profile_path: Path) -> dict[str, Any]:
    profile = load(profile_path)
    validate_profile(profile)
    if model.get("device_profile_sha256") != digest(profile_path):
        raise TimingError("timing model belongs to a different device profile")
    units = workload.get("qemu_units")
    if not isinstance(units, dict):
        raise TimingError("workload needs a qemu_units object")
    coefficients = model.get("coefficients_cycles_per_qemu_unit", {})
    missing = sorted(set(coefficients) - set(units))
    if missing:
        raise TimingError(f"workload is missing calibrated components: {', '.join(missing)}")

    component_cycles = {
        name: float(units[name]) * float(factor) for name, factor in coefficients.items()
    }
    fixed = float(workload.get("fixed_cycles", model.get("fixed_cycles_mean", 0)))
    total = fixed + sum(component_cycles.values())
    if total <= 0:
        raise TimingError("predicted cycle total must be positive")

    clock_name = workload.get("clock_profile", "profile2")
    clocks = {
        item["name"]: int(item["core_hz"])
        for item in profile["clock_profiles"]
        if "name" in item and "core_hz" in item
    }
    if clock_name not in clocks:
        raise TimingError(f"device profile has no measured clock named {clock_name}")
    clock_hz = clocks[clock_name]
    fps = clock_hz / total
    validation = model.get("validation", {})
    error = float(validation.get("max_relative_error", 0))
    bounded = validation.get("sample_count", 0) >= 2
    fps_range = None
    if bounded and error < 1:
        fps_range = {
            "low": clock_hz / (total * (1 + error)),
            "high": clock_hz / (total * (1 - error)) if error else fps,
        }
    return {
        "schema_version": 1,
        "workload": workload.get("name", "unnamed"),
        "clock_profile": clock_name,
        "clock_hz": clock_hz,
        "component_cycles": component_cycles,
        "fixed_cycles": fixed,
        "predicted_cycles_per_frame": total,
        "predicted_fps": fps,
        "fps_range_from_calibration_error": fps_range,
        "validation_status": validation.get("status", "unknown"),
        "claim": model.get("claim"),
    }


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)
    fit_parser = sub.add_parser("fit")
    fit_parser.add_argument("--calibration", type=Path, required=True)
    fit_parser.add_argument("--device-profile", type=Path, required=True)
    fit_parser.add_argument("--output", type=Path)
    pred = sub.add_parser("predict")
    pred.add_argument("--model", type=Path, required=True)
    pred.add_argument("--workload", type=Path, required=True)
    pred.add_argument("--device-profile", type=Path, required=True)
    pred.add_argument("--output", type=Path)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "fit":
            dump(fit(load(args.calibration), args.device_profile), args.output)
        else:
            dump(predict(load(args.model), load(args.workload), args.device_profile), args.output)
        return 0
    except TimingError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
