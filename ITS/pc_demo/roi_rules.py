import json
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


VEHICLE_CLASSES = {"car", "motorcycle", "bus", "truck"}


@dataclass
class Region:
    name: str
    label: str
    points: np.ndarray


def load_roi_config(path, image_shape):
    if not path:
        return [], {}

    config_path = Path(path)
    if not config_path.exists():
        raise FileNotFoundError("ROI config not found: {}".format(config_path))

    with config_path.open("r", encoding="utf-8") as f:
        config = json.load(f)

    height, width = image_shape[:2]
    regions = []
    for item in config.get("regions", []):
        if item.get("type", "polygon") != "polygon":
            continue

        if "points_norm" in item:
            pts = np.array(
                [[x * width, y * height] for x, y in item["points_norm"]],
                dtype=np.float32,
            )
        else:
            pts = np.array(item.get("points", []), dtype=np.float32)

        if pts.shape[0] >= 3:
            regions.append(
                Region(
                    name=item.get("name", "region"),
                    label=item.get("label", item.get("name", "region")),
                    points=pts,
                )
            )

    return regions, config.get("rules", {})


def point_in_region(point, region):
    return cv2.pointPolygonTest(region.points, point, False) >= 0


def object_anchor(det):
    x1, y1, x2, y2 = det["box"]
    cls = det["class_name"]
    if cls == "person" or cls in VEHICLE_CLASSES:
        return ((x1 + x2) * 0.5, y2)
    return ((x1 + x2) * 0.5, (y1 + y2) * 0.5)


def annotate_regions(image, regions):
    for region in regions:
        pts = region.points.astype(np.int32)
        overlay = image.copy()
        cv2.fillPoly(overlay, [pts], (80, 160, 255))
        cv2.addWeighted(overlay, 0.16, image, 0.84, 0, image)
        cv2.polylines(image, [pts], True, (0, 180, 255), 2)
        x, y = pts[0]
        cv2.putText(
            image,
            region.name,
            (int(x), max(20, int(y) - 8)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.62,
            (0, 120, 255),
            2,
            cv2.LINE_AA,
        )


def evaluate_rules(detections, regions, rules):
    region_map = {region.name: region for region in regions}
    region_hits = {region.name: [] for region in regions}

    for det in detections:
        anchor = object_anchor(det)
        det["anchor"] = anchor
        for region in regions:
            if point_in_region(anchor, region):
                region_hits[region.name].append(det)

    warnings = []
    road_hits = region_hits.get("road_lane", [])
    crosswalk_hits = region_hits.get("crosswalk", [])

    if rules.get("person_in_road_lane", True):
        count = sum(1 for det in road_hits if det["class_name"] == "person")
        if count:
            warnings.append(
                {
                    "level": "risk",
                    "code": "person_in_road_lane",
                    "message": "person in motor vehicle lane risk: {}".format(count),
                }
            )

    if rules.get("vehicle_in_crosswalk", True):
        count = sum(1 for det in crosswalk_hits if det["class_name"] in VEHICLE_CLASSES)
        if count:
            warnings.append(
                {
                    "level": "risk",
                    "code": "vehicle_in_crosswalk",
                    "message": "vehicle in pedestrian area risk: {}".format(count),
                }
            )

    if rules.get("mixed_traffic_in_crosswalk", True):
        people = sum(1 for det in crosswalk_hits if det["class_name"] == "person")
        vehicles = sum(1 for det in crosswalk_hits if det["class_name"] in VEHICLE_CLASSES)
        if people and vehicles:
            warnings.append(
                {
                    "level": "risk",
                    "code": "mixed_traffic",
                    "message": "mixed person/vehicle traffic risk: P{} V{}".format(
                        people, vehicles
                    ),
                }
            )

    threshold = int(rules.get("road_vehicle_density_threshold", 0) or 0)
    if threshold > 0:
        count = sum(1 for det in road_hits if det["class_name"] in VEHICLE_CLASSES)
        if count >= threshold:
            warnings.append(
                {
                    "level": "notice",
                    "code": "vehicle_density",
                    "message": "high vehicle density in lane: {}".format(count),
                }
            )

    return region_hits, warnings


def draw_warning_panel(image, counts, warnings):
    x0, y0 = 12, 12
    lines = ["Traffic targets"]
    for name in sorted(counts):
        lines.append("{}: {}".format(name, counts[name]))
    if warnings:
        lines.append("Warnings")
        lines.extend([w["message"] for w in warnings])
    else:
        lines.append("Warnings: none")

    line_h = 24
    width = min(image.shape[1] - 24, 560)
    height = line_h * len(lines) + 14
    overlay = image.copy()
    cv2.rectangle(overlay, (x0, y0), (x0 + width, y0 + height), (20, 20, 20), -1)
    cv2.addWeighted(overlay, 0.62, image, 0.38, 0, image)

    for i, line in enumerate(lines):
        color = (255, 255, 255)
        if i == 0:
            color = (0, 220, 255)
        elif line.startswith("Warnings"):
            color = (0, 180, 255)
        elif "risk" in line:
            color = (0, 80, 255)

        cv2.putText(
            image,
            line,
            (x0 + 10, y0 + 25 + i * line_h),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.62,
            color,
            2,
            cv2.LINE_AA,
        )

