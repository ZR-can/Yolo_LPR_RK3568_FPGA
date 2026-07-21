import argparse
from pathlib import Path

import cv2

from roi_rules import annotate_regions, load_roi_config


def draw_grid(image, step=0.1):
    height, width = image.shape[:2]
    color = (90, 90, 90)
    for i in range(1, int(1 / step)):
        x = int(round(width * i * step))
        y = int(round(height * i * step))
        cv2.line(image, (x, 0), (x, height - 1), color, 1)
        cv2.line(image, (0, y), (width - 1, y), color, 1)
        cv2.putText(
            image,
            "{:.1f}".format(i * step),
            (x + 3, 18),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.45,
            (180, 180, 180),
            1,
            cv2.LINE_AA,
        )
        cv2.putText(
            image,
            "{:.1f}".format(i * step),
            (3, y - 4),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.45,
            (180, 180, 180),
            1,
            cv2.LINE_AA,
        )


def main():
    parser = argparse.ArgumentParser(description="Preview ROI polygons on an image.")
    parser.add_argument("--image", required=True, help="input image path")
    parser.add_argument("--roi-config", required=True, help="ROI JSON config path")
    parser.add_argument("--output", required=True, help="output preview image path")
    parser.add_argument("--grid", action="store_true", help="draw normalized 0.1 grid")
    args = parser.parse_args()

    image = cv2.imread(args.image)
    if image is None:
        raise RuntimeError("failed to read image: {}".format(args.image))

    regions, _ = load_roi_config(args.roi_config, image.shape)
    if args.grid:
        draw_grid(image)
    annotate_regions(image, regions)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(output), image)
    print("regions:", [r.name for r in regions])
    print("saved:", output)


if __name__ == "__main__":
    main()

