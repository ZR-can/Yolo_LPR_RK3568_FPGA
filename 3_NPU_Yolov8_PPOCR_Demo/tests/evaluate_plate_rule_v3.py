#!/usr/bin/env python3

import argparse
import csv
import importlib.util
from collections import Counter
from collections import defaultdict
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DETAILS = (
    REPO_ROOT
    / "3_NPU_Yolov8_PPOCR_Demo"
    / "results"
    / "ppocr_fold_affine_hybrid_mmse_h2_add27_hsw4_board"
    / "details.tsv",
    REPO_ROOT
    / "3_NPU_Yolov8_PPOCR_Demo"
    / "results"
    / "ppocr_fold_affine_fp16_board"
    / "details.tsv",
)


def load_plate_rule():
    rule_path = (
        REPO_ROOT
        / "1_PC_Training"
        / "PaddleOCR"
        / "ppocr"
        / "metrics"
        / "plate_rule.py"
    )
    spec = importlib.util.spec_from_file_location(
        "ga36_plate_rule_for_replay", rule_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def classify_outcome(label, raw_prediction, corrected_prediction):
    raw_correct = raw_prediction == label
    corrected = corrected_prediction == label
    if raw_correct and corrected:
        return "kept_correct"
    if not raw_correct and corrected:
        return "fixed"
    if raw_correct and not corrected:
        return "harmed"
    if raw_prediction != corrected_prediction:
        return "changed_still_wrong"
    return "unchanged_still_wrong"


def replay(details_path, plate_rule):
    with details_path.open(encoding="utf-8-sig", newline="") as details_file:
        rows = list(csv.DictReader(details_file, delimiter="\t"))

    totals = Counter()
    groups = defaultdict(Counter)
    examples = defaultdict(list)
    for row in rows:
        label = row["label"]
        raw_prediction = row["prediction"]
        is_green_plate = len(label) == 8
        corrected_prediction = (
            plate_rule.correct_plate_prediction_for_pipeline(
                raw_prediction, is_green_plate))
        corrected_prediction = plate_rule.truncate_plate_prediction(
            corrected_prediction, is_green_plate)
        outcome = classify_outcome(
            label, raw_prediction, corrected_prediction)

        totals["rows"] += 1
        totals["raw_correct"] += int(raw_prediction == label)
        totals["corrected_correct"] += int(corrected_prediction == label)
        totals["changed"] += int(raw_prediction != corrected_prediction)
        totals[outcome] += 1

        plate_kind = "green" if is_green_plate else "non_green"
        group = groups[(row["subset"], plate_kind)]
        group["rows"] += 1
        group["raw_correct"] += int(raw_prediction == label)
        group["corrected_correct"] += int(corrected_prediction == label)
        group["changed"] += int(raw_prediction != corrected_prediction)
        group[outcome] += 1

        if len(examples[outcome]) < 5:
            examples[outcome].append((
                row["subset"],
                row["image"],
                label,
                raw_prediction,
                corrected_prediction,
            ))

    return totals, groups, examples


def percentage(numerator, denominator):
    return 100.0 * numerator / denominator if denominator else 0.0


def print_result(details_path, totals, groups, examples):
    raw_accuracy = percentage(totals["raw_correct"], totals["rows"])
    corrected_accuracy = percentage(
        totals["corrected_correct"], totals["rows"])
    print(f"\n[{details_path.parent.name}]")
    print(
        f"rows={totals['rows']} "
        f"raw={totals['raw_correct']} ({raw_accuracy:.6f}%) "
        f"corrected={totals['corrected_correct']} "
        f"({corrected_accuracy:.6f}%) "
        f"gain={corrected_accuracy - raw_accuracy:+.6f}pp")
    print(
        f"changed={totals['changed']} fixed={totals['fixed']} "
        f"harmed={totals['harmed']} "
        f"changed_still_wrong={totals['changed_still_wrong']} "
        f"unchanged_still_wrong={totals['unchanged_still_wrong']}")

    for (subset, plate_kind), group in sorted(groups.items()):
        raw_accuracy = percentage(group["raw_correct"], group["rows"])
        corrected_accuracy = percentage(
            group["corrected_correct"], group["rows"])
        print(
            f"  {subset}/{plate_kind}: rows={group['rows']} "
            f"{raw_accuracy:.6f}% -> {corrected_accuracy:.6f}% "
            f"fixed={group['fixed']} harmed={group['harmed']} "
            f"changed_still_wrong={group['changed_still_wrong']}")

    for outcome in ("fixed", "harmed", "changed_still_wrong"):
        print(f"  {outcome} examples:")
        for subset, image, label, raw, corrected in examples[outcome]:
            print(
                f"    {subset}\t{image}\t{label}\t{raw}\t{corrected}")


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Replay ga36_plate_type_v3 with deployment correction and "
            "truncation against PP-OCR details.tsv logs."))
    parser.add_argument(
        "details",
        nargs="*",
        type=Path,
        default=DEFAULT_DETAILS,
        help="details.tsv files; defaults to the two selected board models",
    )
    args = parser.parse_args()

    plate_rule = load_plate_rule()
    print(f"rule={plate_rule.NORMALIZATION_VERSION}")
    print(
        "green proxy: label length 8 "
        "(details.tsv does not contain the detector plate class)")
    for details_path in args.details:
        totals, groups, examples = replay(details_path, plate_rule)
        print_result(details_path, totals, groups, examples)


if __name__ == "__main__":
    main()
