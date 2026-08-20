# YOLOv8-OBB 标准数据集配置（2026-07-31）

> 本文档覆盖旧版过程记录，定义当前 `yolo_obb` 分支唯一有效的数据集标准。
> 当前训练已经使用该版本启动，训练结束前不得修改其图片、标签、划分或
> `val_balanced.txt`。

## 1. 标准版本与权威文件

- 构建随机种子：`20260731`。
- 数据集根目录：
  `D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/datasets/yolo_obb_640`。
- 图片保存原始分辨率，派生目录使用同卷 NTFS 硬链接，不复制图片数据块。
- `imgsz=640` 仅由训练阶段执行 letterbox，不预先缩放或重编码源图。
- 当前数据集由以下文件共同定义：
  - [构建脚本](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/scripts/build_yolo_obb_dataset.py)
  - [YOLO 数据配置](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/configs/yolo_obb_config.yaml)
  - [构建摘要](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/datasets/yolo_obb_640/build_summary.json)
  - [数据清单](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/datasets/yolo_obb_640/dataset_manifest.tsv)
  - [平衡验证清单](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/datasets/yolo_obb_640/val_balanced.txt)

训练入口配置固定为：

```yaml
path: D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/datasets/yolo_obb_640
train: images/train
val: val_balanced.txt
test: images/test

names:
  0: blue
  1: green
  2: yellow_single
  3: other
```

完整 `images/val` 是审计池；实际训练验证只使用
`val_balanced.txt`。最终测试使用未做平衡抽样的 `images/test`。

## 2. OBB 标签标准

每个目标一行，共 9 列：

```text
class_id x1 y1 x2 y2 x3 y3 x4 y4
```

- 四点坐标均归一化到 `[0, 1]`。
- 四个角点按稳定顺时针顺序写出。
- CCPD2019、CCPD2020 从文件名解析四角点。
- CRPD 从 `x1 y1 ... x4 y4 type content` 原始标注解析四角点。
- 现有特殊车牌的水平框转换为零角度 OBB。
- 类别映射固定为：
  - CCPD2019：`blue`。
  - CCPD2020 `ccpd_green`：`green`。
  - CRPD `type=0 -> blue`。
  - CRPD `type=1 -> yellow_single`。
  - CRPD `type=2`：对象级删除，不进入标签。
  - CRPD `type=3 -> other`。
  - 教练、香港出入境、澳门出入境、警用车牌：`other`。

## 3. 原始数据源与抽样规则

### 3.1 CCPD2019

CCPD2019 使用稳定哈希完成 80/10/10 的 train/val/test 划分。各子集最终
数量如下：

| 子集 | train | val | test |
|---|---:|---:|---:|
| `ccpd_base` | 2,000 | 250 | 250 |
| `ccpd_blur` | 1,000 | 125 | 125 |
| `ccpd_challenge` | 2,000 | 250 | 250 |
| `ccpd_db` | 1,000 | 125 | 125 |
| `ccpd_fn` | 2,000 | 250 | 250 |
| `ccpd_rotate` | 2,000 | 250 | 250 |
| `ccpd_tilt` | 2,500 | 312 | 312 |
| `ccpd_weather` | 500 | 62 | 62 |
| `ccpd_np` 无牌负样本 | 500 | 62 | 62 |

`ccpd_np` 中 7 个扩展名为 `.jpg`、实际为 macOS AppleDouble 元数据的
文件在抽样前排除，再按相同稳定规则选择有效替补。因此最终负样本配额不变。

### 3.2 CCPD2020

- 仅使用 `CCPD2020/ccpd_green`。
- 保持其原始 train/val/test，不重新划分。
- 最终数量为 train 5,769、val 1,001、test 5,006。
- 所有目标映射为 `green`。

### 3.3 CRPD

- `CRPD_multi` 和 `CRPD_double` 原则上全部保留。
- `CRPD_single` 的唯一 train 源图目标为 10,000 张。
- 从 `CRPD_single` 选择远距离蓝牌时，按 640 投影后的车牌最小短边分层：
  `<12 px=50%`、`12～16 px=30%`、`>=16 px=20%`。
- `type=2` 目标全部删除：
  - `CRPD_single` 删除后为空的图片不收录。
  - `CRPD_multi`、`CRPD_double` 删除后为空的有效图片可作为负样本保留。
- `content` 缺失但四点坐标和 `type` 完整的目标保留，因为检测训练不使用
  车牌文字。
- 最终唯一源图数量：

| 子集 | train | val | test |
|---|---:|---:|---:|
| `CRPD_double` | 4,000 | 1,000 | 1,102 |
| `CRPD_multi` | 996 | 250 | 335 |
| `CRPD_single` | 10,000 | 4,986 | 1,069 |

### 3.4 现有特殊车牌

- 共 328 张唯一源图。
- 使用稳定的 60/30/10 划分：train 197、val 98、test 33。
- train 中每张总计出现 4 次；val/test 不重复。
- 四个来源分别为教练、香港出入境、澳门出入境和警用车牌。

## 4. 训练重复权重

重复只发生在 train，且是同一源图的额外硬链接，不会跨 split：

- 含 `other` 的图片总计 4 份。
- 不含 `other`、但需要增强 `yellow_single` 的图片总计 2 份。
- 同时包含 `other` 与 `yellow_single` 时，以 `other` 的 4 份规则为准。
- 其他图片保持 1 份。

| 每个源图总份数 | 唯一源图数 |
|---:|---:|
| 1 | 29,241 |
| 2 | 4,602 |
| 4 | 619 |

重复前后 train 框分布为：

| 类别 | 唯一源图框数 | 唯一占比 | 训练加权框数 | 加权占比 |
|---|---:|---:|---:|---:|
| `blue` | 29,098 | 72.52% | 29,815 | 62.98% |
| `green` | 5,769 | 14.38% | 5,769 | 12.19% |
| `yellow_single` | 4,639 | 11.56% | 9,282 | 19.61% |
| `other` | 619 | 1.54% | 2,476 | 5.23% |
| 合计 | 40,125 | 100% | 47,342 | 100% |

额外训练副本共 6,459 张。该策略显著提高了稀有类别权重，但不会增加其
真实场景多样性。

## 5. 最终落盘规模

| split | 图片/标签记录 | 唯一源图 | OBB 框 | 负样本 |
|---|---:|---:|---:|---:|
| train | 40,921 | 34,462 | 47,342 | 500 |
| val 完整审计池 | 9,021 | 9,021 | 10,504 | 62 |
| test | 9,231 | 9,231 | 11,010 | 62 |
| 合计 | 59,173 | 52,714 | 68,856 | 624 |

各 split 的实际图片数和 `.txt` 标签数完全相等。train 的 500 张负样本占
训练图片的 1.22%。

按数据来源统计：

| split | CCPD2019 | CCPD2020 | CRPD | 特殊车牌 |
|---|---:|---:|---:|---:|
| train 加权记录 | 13,500 | 5,769 | 20,864 | 788 |
| train 唯一源图 | 13,500 | 5,769 | 14,996 | 197 |
| val 完整池 | 1,686 | 1,001 | 6,236 | 98 |
| test | 1,686 | 5,006 | 2,506 | 33 |

test 框类别为 `blue=5,711`、`green=5,006`、
`yellow_single=256`、`other=37`。因此 test 可用于总体回归，但
`other` 和黄牌的测试置信区间会明显大于常见类别。

## 6. 平衡验证集标准

`val_balanced.txt` 是从完整 val 池确定性选择的 2,918 张唯一图片：

- 总框数：3,107。
- 负样本：62 张，全部保留。
- 框类别：`blue=1,000`、`green=1,000`、
  `yellow_single=1,002`、`other=105`。
- `other` 使用完整 val 中的全部真实目标，不通过重复图片伪造验证权重。
- 来源：CCPD2019=256、CCPD2020=1,000、CRPD=1,564、
  特殊车牌=98。
- 清单无重复路径，全部指向 `images/val`，且与 train/test 无源图交叉。

平衡 val 的正样本按 640 投影短边分布：

| 最小短边 | 图片数 | 正样本占比 |
|---|---:|---:|
| `<8 px` | 32 | 1.12% |
| `8～12 px` | 341 | 11.94% |
| `12～16 px` | 931 | 32.60% |
| `16～24 px` | 354 | 12.39% |
| `24～32 px` | 38 | 1.33% |
| `>=32 px` | 1,160 | 40.62% |

正样本中短边 `<16 px` 的图片共 1,304 张，占 45.66%。train 加权正样本中
短边 `<16 px` 的图片共 16,245/40,421，占 40.19%。数据覆盖了大量远距离
小车牌，但最终仍必须单独报告逐尺寸指标，不能仅依赖总体 mAP。

## 7. 异常数据处理

当前标准采用“删除不可靠样本，不自动猜测修复”的原则：

- 排除 CCPD2019 `ccpd_np`：
  `1982.jpg`、`2705.jpg`、`3213.jpg`、`3922.jpg`、
  `4205.jpg`、`4626.jpg`、`5830.jpg`。这些文件实际为 AppleDouble
  元数据，原始文件保留，派生数据集不收录。
- 排除 CRPD：
  - `CRPD_multi/train/44_0191.jpg`
  - `CRPD_multi/train/44_1333.jpg`
  - `CRPD_multi/train/45_0089.jpg`
  - `CRPD_multi/train/45_1404.jpg`
  - `CRPD_single/val/48_0951.jpg`
- 前 4 张 CRPD 图片为空标签但实际可见车牌，不能作为负样本；最后 1 张存在
  重复角点，自动最小矩形结果不可靠。
- CRPD 共删除 76 个 `type=2` 目标：train/val/test 为 49/11/16。
- 10 个 `content` 缺失但几何和类型有效的 CRPD 目标保留。
- 退化四点自动修复计数为 0；不可靠退化样本直接排除。
- 3 张 CCPD2020 图片的文件名角点超出右边界 1～22 px，构建时裁剪到图像
  边界；原图车牌本身位于画面边缘，属于合理边界裁剪。

验收结果：

- 52,714 个唯一源图全部通过 Pillow 解码检查。
- JPEG 同时通过 EOI 完整性检查，异常数为 0。
- 所有派生图片均已验证为源图的 NTFS 硬链接。
- 所有 OBB 标签均为 9 列，类别和坐标范围合法。
- 同一源图跨 train/val/test 泄漏数为 0。