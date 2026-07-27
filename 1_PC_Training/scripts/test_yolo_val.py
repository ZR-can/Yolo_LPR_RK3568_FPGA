"""
CCPD测试集评估脚本
功能：加载训练好的模型，评估test集，输出mAP/Precision/Recall
"""
from pathlib import Path
import csv

import numpy as np
from ultralytics import YOLO


# ============================================================
# 1. 路径配置
# ============================================================

MODEL_PATH = (
    r"D:\Yolo_LPR_RK3568_FPGA_Project"
    r"\1_PC_Training\scripts\runs\train_results"
    r"\yolov8n_multi_class\weights\best.pt"
)

DATA_PATH = (
    r"D:\Yolo_LPR_RK3568_FPGA_Project"
    r"\1_PC_Training\configs\yolo_config.yaml"
)

PROJECT_PATH = (
    r"D:\Yolo_LPR_RK3568_FPGA_Project"
    r"\1_PC_Training\scripts\runs\test_results"
)

RUN_NAME = "yolov8n_ap_by_iou"


def get_class_name(names, class_id: int) -> str:
    """
    兼容model.names为dict或list两种形式。
    """
    if isinstance(names, dict):
        return str(names[class_id])

    return str(names[class_id])


def main() -> None:
    # ========================================================
    # 2. 加载模型
    # ========================================================

    model = YOLO(MODEL_PATH)

    # ========================================================
    # 3. 验证测试集
    # ========================================================

    results = model.val(
        data=DATA_PATH,
        split="test",
        save_txt=True,
        plots=True,
        device="0",
        project=PROJECT_PATH,
        name=RUN_NAME,

        # 正式计算AP/mAP时建议使用较低置信度阈值，
        # 以保留完整PR曲线上的候选预测。
        conf=0.45,

        # 这里是NMS使用的IoU阈值，
        # 不是AP评估中的0.50～0.95阈值。
        iou=0.7,
    )

    # ========================================================
    # 4. 获取AP矩阵
    #
    # all_ap.shape = (有效类别数, 10)
    #
    # 每一列依次对应：
    # IoU = 0.50, 0.55, 0.60, ..., 0.95
    # ========================================================

    all_ap = np.asarray(results.box.all_ap, dtype=np.float64)

    class_ids = np.asarray(
        results.box.ap_class_index,
        dtype=np.int64,
    )

    iou_thresholds = np.linspace(0.50, 0.95, 10)

    # ========================================================
    # 5. 数据检查
    # ========================================================

    if all_ap.ndim != 2:
        raise RuntimeError(
            f"all_ap维度错误：{all_ap.shape}，"
            "预期为二维矩阵(num_classes, 10)"
        )

    if all_ap.shape[1] != len(iou_thresholds):
        raise RuntimeError(
            f"all_ap列数错误：{all_ap.shape[1]}，"
            f"预期为{len(iou_thresholds)}"
        )

    if all_ap.shape[0] != len(class_ids):
        raise RuntimeError(
            f"AP矩阵类别数{all_ap.shape[0]}与"
            f"ap_class_index长度{len(class_ids)}不一致"
        )

    # ========================================================
    # 6. 输出每个类别在各IoU阈值下的AP
    # ========================================================

    print("\n" + "=" * 100)
    print("各类别在IoU=0.50～0.95下的AP")
    print("=" * 100)

    header = (
        f"{'Class':<18}"
        + "".join(
            f"{f'AP@{iou:.2f}':>10}"
            for iou in iou_thresholds
        )
        + f"{'AP@0.50:0.95':>16}"
    )

    print(header)
    print("-" * len(header))

    csv_rows = []

    for row_index, class_id in enumerate(class_ids):
        class_name = get_class_name(
            model.names,
            int(class_id),
        )

        class_ap_values = all_ap[row_index]

        # 单个类别在10个IoU阈值上的平均AP
        class_ap50_95 = float(class_ap_values.mean())

        output_line = f"{class_name:<18}"

        for ap_value in class_ap_values:
            output_line += f"{ap_value:>10.4f}"

        output_line += f"{class_ap50_95:>16.4f}"

        print(output_line)

        csv_rows.append(
            {
                "class_id": int(class_id),
                "class_name": class_name,
                **{
                    f"AP@{iou:.2f}": float(ap_value)
                    for iou, ap_value in zip(
                        iou_thresholds,
                        class_ap_values,
                    )
                },
                "AP@0.50:0.95": class_ap50_95,
            }
        )

    # ========================================================
    # 7. 计算所有类别在各IoU阈值下的总体mAP
    # ========================================================

    map_per_iou = all_ap.mean(axis=0)
    map50_95 = float(map_per_iou.mean())

    print("\n" + "=" * 100)
    print("所有类别在各IoU阈值下的总体mAP")
    print("=" * 100)

    for iou, map_value in zip(
        iou_thresholds,
        map_per_iou,
    ):
        print(f"mAP@{iou:.2f}: {map_value:.4f}")

    print("-" * 40)
    print(f"mAP@0.50:0.95:       {map50_95:.4f}")
    print(f"Ultralytics box.map: {results.box.map:.4f}")

    # ========================================================
    # 8. 计算核心三类指标，排除other
    # ========================================================

    core_class_names = {
        "blue",
        "green",
        "yellow_single",
    }

    core_row_indices = []

    for row_index, class_id in enumerate(class_ids):
        class_name = get_class_name(
            model.names,
            int(class_id),
        )

        if class_name in core_class_names:
            core_row_indices.append(row_index)

    if core_row_indices:
        core_ap = all_ap[core_row_indices]

        # 每个IoU阈值下，对核心三类求平均
        core_map_per_iou = core_ap.mean(axis=0)

        # 核心三类在0.50～0.95上的总平均
        core_map50_95 = float(core_map_per_iou.mean())

        print("\n" + "=" * 100)
        print("核心三类mAP（排除other）")
        print("=" * 100)

        for iou, map_value in zip(
            iou_thresholds,
            core_map_per_iou,
        ):
            print(
                f"核心三类 mAP@{iou:.2f}: "
                f"{map_value:.4f} "
                f"({map_value * 100:.2f}%)"
            )

        print("-" * 50)
        print(
            "核心三类 mAP@0.50:0.95: "
            f"{core_map50_95:.4f} "
            f"({core_map50_95 * 100:.2f}%)"
        )

    else:
        print(
            "\n警告：没有找到blue、green、"
            "yellow_single三个核心类别。"
        )

    # ========================================================
    # 9. 输出各类别Precision、Recall和F1
    # ========================================================

    precisions = np.asarray(
        results.box.p,
        dtype=np.float64,
    )

    recalls = np.asarray(
        results.box.r,
        dtype=np.float64,
    )

    f1_scores = np.asarray(
        results.box.f1,
        dtype=np.float64,
    )

    print("\n" + "=" * 100)
    print("各类别综合检测指标")
    print("=" * 100)

    print(
        f"{'Class':<18}"
        f"{'Precision':>12}"
        f"{'Recall':>12}"
        f"{'F1':>12}"
        f"{'AP@0.50':>12}"
        f"{'AP@0.50:0.95':>18}"
    )

    print("-" * 84)

    for row_index, class_id in enumerate(class_ids):
        class_name = get_class_name(
            model.names,
            int(class_id),
        )

        print(
            f"{class_name:<18}"
            f"{precisions[row_index]:>12.4f}"
            f"{recalls[row_index]:>12.4f}"
            f"{f1_scores[row_index]:>12.4f}"
            f"{all_ap[row_index, 0]:>12.4f}"
            f"{all_ap[row_index].mean():>18.4f}"
        )

    # ========================================================
    # 10. 保存各类别AP结果到CSV
    # ========================================================

    save_directory = Path(PROJECT_PATH) / RUN_NAME
    save_directory.mkdir(
        parents=True,
        exist_ok=True,
    )

    csv_path = save_directory / "ap_by_class_and_iou.csv"

    csv_fieldnames = [
        "class_id",
        "class_name",
        *[
            f"AP@{iou:.2f}"
            for iou in iou_thresholds
        ],
        "AP@0.50:0.95",
    ]

    with csv_path.open(
        mode="w",
        newline="",
        encoding="utf-8-sig",
    ) as csv_file:
        writer = csv.DictWriter(
            csv_file,
            fieldnames=csv_fieldnames,
        )

        writer.writeheader()
        writer.writerows(csv_rows)

    print("\n" + "=" * 100)
    print(f"各类别AP结果已保存至：\n{csv_path.resolve()}")
    print("=" * 100)


if __name__ == "__main__":
    main()
