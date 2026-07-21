import argparse
import json
from pathlib import Path

import cv2


def parse_points(text):
    points = []
    if not text:
        return points
    for item in text.split(";"):
        item = item.strip()
        if not item:
            continue
        x_str, y_str = item.split(",", 1)
        points.append([float(x_str), float(y_str)])
    if len(points) < 3:
        raise ValueError("a polygon needs at least 3 points: {}".format(text))
    return points


def normalize(points, width, height):
    return [[round(x / width, 6), round(y / height, 6)] for x, y in points]


def add_region(regions, name, label, points_text, width, height):
    if not points_text:
        return
    regions.append(
        {
            "name": name,
            "label": label,
            "type": "polygon",
            "points_norm": normalize(parse_points(points_text), width, height),
        }
    )


def main():
    parser = argparse.ArgumentParser(
        description="Create normalized ROI config from pixel coordinates."
    )
    parser.add_argument("--image", required=True, help="reference image path")
    parser.add_argument("--output", required=True, help="output JSON path")
    parser.add_argument(
        "--road",
        default="",
        help='road lane polygon pixels, for example "100,420;1180,390;1270,720;0,720"',
    )
    parser.add_argument(
        "--crosswalk",
        default="",
        help='crosswalk polygon pixels, for example "420,330;900,330;1080,500;260,520"',
    )
    parser.add_argument(
        "--vehicle-threshold",
        type=int,
        default=4,
        help="vehicle density warning threshold in road_lane",
    )
    args = parser.parse_args()

    image = cv2.imread(args.image)
    if image is None:
        raise RuntimeError("failed to read image: {}".format(args.image))

    height, width = image.shape[:2]
    regions = []
    add_region(regions, "road_lane", "motor vehicle lane", args.road, width, height)
    add_region(regions, "crosswalk", "crosswalk / pedestrian area", args.crosswalk, width, height)

    config = {
        "image_size_hint": [width, height],
        "regions": regions,
        "rules": {
            "person_in_road_lane": True,
            "vehicle_in_crosswalk": True,
            "mixed_traffic_in_crosswalk": True,
            "road_vehicle_density_threshold": args.vehicle_threshold,
        },
    }

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print("image:", args.image)
    print("size:", width, height)
    print("regions:", [r["name"] for r in regions])
    print("saved:", output)


if __name__ == "__main__":
    main()

