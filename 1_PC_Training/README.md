# 1_PC_Training

## 功能说明

该目录用于完成训练前与训练中的 PC 侧工作，主要包括：

- 数据集整理、筛选、格式转换
- YOLOv8 车牌检测模型训练与验证
- LPRNet 车牌字符识别模型训练与验证
- 训练结果、权重与测试输出管理

## 目录结构

- `requirements.txt`
  Python 依赖列表
- `configs/`
  YOLO 训练配置，例如 `yolo_config.yaml`
- `datasets/`
  本地训练与验证数据集
- `scripts/`
  YOLO 数据处理、统计、训练与验证脚本
- `LPRNet_Pytorch/`
  LPRNet 训练、测试、模型与权重

## 运行环境

- 默认使用 conda 环境：`YOLOv8n_LPRNet`
- 如需补装依赖，可在本目录执行：

```bash
pip install -r requirements.txt
```

## 常用使用方法

### PP-OCRv4 Linux RTX 4090 训练更新（2026-07-17）

- 新训练配置从官方 `ch_PP-OCRv4_rec` 权重开始，不恢复旧 `best_accuracy`。
- 输入保持 48×160，网络保持 PPLCNetV3 0.95 + SVTR + CTC/NRTR MultiHead，推理使用 CTC。
- CBLPRD 最终清单为训练 251,192 张、验证 17,357 张，共 268,549 张。
- 配置显式读取 basic、hard 和 6 类 special 共 8 份清单；首轮基线训练和验证均使用全部数据，Linux 数据集绝对路径为 `/root/autodl-tmp/CBLPRD`。
- RTX 4090 默认 batch 512、12 workers、AMP O1、学习率 `3e-4`、3 epoch warmup 和 40 epochs。

完整的数据放置、试跑、全量训练和续训说明见 `README_CBLPRD_RK3568.md`。

2026-07-18 更新 `scripts/build_PPOCRv4_cblprd_manifests.py`：按当前外层清单、内层 `CBLPRD/basic|hard|special` 的目录结构生成 18 份清单，并在写入前双向核对所有 JPG 与源标签，防止出现只生成 special、遗漏 basic/hard 清单的情况。

2026-07-18 增加 CTC SVTR + FC 微调配置：训练入口按 `Finetune.trainable_prefixes` 只解冻 `head.ctc_encoder.*` 与 `head.ctc_head.fc.*`，并强制全部 BN 使用冻结的全局统计；`scripts/eval_ppocr_cblprd_subsets.sh` 可对 basic、hard 和 6 类 special 分别评估并集中打印准确率。详细命令见 `README_CBLPRD_RK3568.md`。

首轮服务器实测在 `3e-6`、global step 300 达到联合准确率 88.4081%，其中 hard 为 66.9469%；相对 CTC FC-only 最佳结果提升 3.1227 个百分点。继续训练会明显回落，当前以 step 300 保存的 `best_accuracy` 为有效 checkpoint，详细分子集结果和后续实验边界见训练说明。

2026-07-18 增加独立 `PlateRuleRecMetric`：按数据审计阶段相同的 GA 36 位置规则修正预测中的 `0/O`、`1/I`，同时报告 `raw_acc`、规则修正后 `acc`、修正命中数和误伤数；标准 `RecMetric` 不变。首次服务器回放发现旧规则对领牌误伤208条、对警牌误伤2条，现已升级为按号牌类型分支的 `ga36_plate_type_io_v2`；本地全部训练及验证清单共268,549条GT经新规则处理全部保持不变。

2026-07-21 补录原始 `student_cblprd73` 的 8 子集规则评估：17,357 张验证图的原始整牌准确率为 85.1875%，`ga36_plate_type_io_v2` 修正后为 90.6435%，共修正 947 张且误伤为 0；hard 子集原始准确率仅 51.9912%。完整分子集结果见 `README_CBLPRD_RK3568.md`。

2026-07-18 增加 `scripts/eval_onnx_ppocr_cblprd_subsets.py`：使用与PaddleOCR一致的48×160 BGR预处理、73字符CTC解码和集中维护的 `plate_rule.py`，直接对固定输入ONNX执行8个验证子集及TOTAL评估，并可输出逐图TSV明细。

2026-07-23 将训练、ONNX 评估与 RK3568 部署共用的修正规则升级为 `ga36_plate_type_v3`：使馆数字结构不再要求省份前缀，警牌第二位恢复为 `A-Z` 发牌机关规则并允许合法 `I/O`，序号位置继续执行 `O/I -> 0/1`。既有 `ga36_plate_type_io_v2` 准确率属于历史结果，必须以 v3 重新评估后才能横向比较。

2026-07-18 在 `datasets/yolo_lprnet_crops/` 增加 `eval_onnx_blue_green_train_test.py`：直接遍历蓝牌/绿牌的train与test裁剪图，以文件名作为GT，复用相同的ONNX预处理、CTC解码和规则修正并集中输出四组及TOTAL准确率。当前实际图片为blue_train 13,106、green_train 1,913、blue_test 1,404、green_test 608，共17,031张；`classified_txts/yolo_crops_train.txt`与`yolo_crops_test.txt`已按实际图片重建为15,019和2,012条，路径、文件名GT和牌照类型逐条对应，旧test清单中的26条缺图绿牌记录及重复指向train的val清单已移除。

2026-07-23 增加 `datasets/yolo_lprnet_crops/eval_onnx_blue_green_train_test_letterbox.py`：保持原评估流程不变，只将预处理替换为固定左对齐的等比例 letterbox；图像限制在 `160×48` 内，窄图在右侧补零，宽图在上下居中补零，左侧始终不补。`94×24` 样本对应有效区域 `160×41`，上补3行、下补4行，可与原脚本的直接 `160×48` 缩放结果进行同口径对照。

### 1. 数据处理

`scripts/` 中包含多种数据处理脚本，例如：

- `process_ccpd.py`
- `process_crpd.py`
- `process_cblprd.py`
- `split_lprnet_dataset.py`
- `yolo_lprnet_crops.py`

这些脚本用于生成 YOLO 检测数据、LPRNet 识别数据，或把 YOLO 检测结果裁剪成 LPRNet 训练样本。

2026-07-26 增加 `scripts/analyze_yolo_finetune_dataset.py`：在追加特殊车牌并微调
YOLO 前，逐张核对原 `yolo_format` 与 `特殊车牌` 的图片解码、图片/标签配对、
YOLO 框格式及边界，并统计类别、尺寸、跨划分同名和完全重复图片。特殊车牌四个
子目录内的局部 `0=other` 只用于源数据，合并到现有四分类时必须重映射为全局
`3=other`。

```bash
cd 1_PC_Training
python scripts/analyze_yolo_finetune_dataset.py
```

`scripts/prepare_yolo_finetune_dataset.py` 根据上述审计结果构建独立的
`datasets/yolo_finetune_special/`，不改写原 `yolo_format`。脚本会：

- 将特殊车牌的局部 `0=other` 重映射为全局 `3=other`；
- 按四个特殊子类分别做固定种子的 8:1:1 分层划分；
- 仅在 train 中把新增样本重复为 3 份，借助每轮随机增强缓解 `other` 欠采样；
- 按 `test > val > train` 排除旧集跨划分的完全重复图片；
- 合并 IoU 不低于 0.95 的重复框，并在派生标签中裁剪旧集越界框；
- 使用 NTFS 硬链接复用图片数据块，同时生成 TSV 清单和 JSON 构建摘要。

```bash
python scripts/prepare_yolo_finetune_dataset.py --dry-run
python scripts/prepare_yolo_finetune_dataset.py --apply
```

2026-07-26 实际构建结果：审计 20,219 张旧图和 328 张新增图均可解码且
图片/标签成对；新增集无格式或越界异常。旧集发现 1 个越界框和 30 组跨划分
完全重复图，均仅在派生集中修正/排除。派生集 train/val/test 分别为
13,012/3,826/4,203 张，框数为 14,835/4,246/4,801；`other` 框数分别为
1,208/40/37。新增 328 张唯一源图按四个子类分层划为 262/33/33 张，train
中的新增源图重复 3 次，val/test 不重复。

#### CBLPRD-330k 转 PP-OCRv4 单层车牌数据集

`scripts/process_PPOCRv4_cblprd.py` 按原始 `train.txt` 和 `val.txt` 划分，只保留以下类别：

- 黑色车牌
- 单层黄牌
- 普通蓝牌
- 新能源大型车
- 新能源小型车

先执行不写文件的完整检查：

```bash
conda activate YOLOv8n_LPRNet
python scripts/process_PPOCRv4_cblprd.py --dry-run
```

检查通过后，显式确认在原数据集内执行不可逆整理：

```bash
python scripts/process_PPOCRv4_cblprd.py --apply
```

脚本不会复制或重编码图片。保留图片通过同盘移动分别进入 `train/`、`val/`；双层黄牌和拖拉机绿牌图片永久删除；原 `data.txt` 和清空后的旧图片目录被移除。最终直接将原目录整理为：

```text
CBLPRD-330k/
├── train/
├── val/
├── train.txt
└── val.txt
```

`train.txt` 和 `val.txt` 每行均为 PaddleOCR 识别数据标准格式：

```text
train/000272981.jpg\t粤Z31632D
```

PaddleOCR 配置中将 `data_dir` 指向该数据集根目录，并分别将训练、评估的 `label_file_list` 指向 `train.txt`、`val.txt`。脚本不会再次缩放或裁切图片。`--apply` 操作不可逆，必须先确认 `--dry-run` 的统计结果正确，并确保不再需要被排除的原图及旧标签。

#### 从 CRPD 追加白色警牌

`scripts/process_PPOCRv4_crpd_white.py` 遍历 `CRPD_single`、`CRPD_double`、`CRPD_multi` 的 train/val/test，收集类别 3 候选。脚本不会直接信任 CRPD 的车牌文字，而是将候选框透视矫正后，与 `lprnet_7char/classified_txts/type_白色车牌_train.txt` 和 `type_白色车牌_val.txt` 中的可信裁剪图做全局图像匹配。输出标签始终采用可信清单中的车牌文字和 train/val 划分。

先执行只读配对检查：

```bash
python scripts/process_PPOCRv4_crpd_white.py --dry-run
```

确认配对数量、误差和纠错项正确后追加：

```bash
python scripts/process_PPOCRv4_crpd_white.py --apply
```

新增裁剪使用 CRPD 原始四点标注进行透视矫正，输出为 `128x48` JPEG，直接写入现有 `CBLPRD-330k/train/`、`val/` 并以 PaddleOCR 的 `路径<TAB>标签` 格式追加标签。输出文件名包含 CRPD 来源信息，不会因同号警车重复出现而相互覆盖。

当前本地数据只读检查结果：CRPD 类别 3 共 433 个候选，可信警牌共 212 张（train 167、val 45），全部完成唯一匹配；最大最佳 MSE 为 45.33，最小最佳/次佳间隔为 328.55。可信清单还纠正了 CRPD 中 5 条漏写“警”的文字标签：`川A3228`、`川A3260`、`川A3316`、`川A6337`、`川F1209`。

注意：`lprnet_7char/classified_txts/type_白色车牌_*.txt` 保留的是旧 LPRNet 三字段格式 `路径 空格 标签 空格 类型`，仅作为可信配对输入，不能配置为 PaddleOCR 的 `label_file_list`。PaddleOCR 训练只使用 `CBLPRD-330k/train.txt` 和 `val.txt`，这两份最终标签会被脚本严格校验为 `.jpg<TAB>车牌号`。

如需单独审计或修复最终 PP-OCR 标签中的空格分隔、多余 TAB，可使用：

```bash
python scripts/normalize_PPOCR_labels.py --dry-run
python scripts/normalize_PPOCR_labels.py --apply
```

规范化脚本只接受能够无歧义解析为“`.jpg` 路径 + 单个车牌号”的行；遇到三字段、重复路径或无法判断的内容会停止，不会猜测性改写。写入前还会检查文件是否被其他程序修改，并通过临时文件原子替换。

上次人工同步完成时的记录为：`train.txt` 275,130 行、`val.txt` 14,484 行，全部严格为 `.jpg<TAB>车牌号`。人工目视筛选后保留白色警牌 train 110 张、val 24 张，共 134 张；淘汰的 train 57 张、val 21 张共 78 张记录在 `crpd_white_rejected.txt`，CRPD 警牌追加脚本不会重新生成。后续若继续手工删图，需要再次运行下述同步命令更新当前统计。

人工目视删除低质量图片后，使用以下命令按现存图片同步标签：

```bash
python scripts/sync_PPOCR_labels_with_images.py --dry-run
python scripts/sync_PPOCR_labels_with_images.py --apply
```

同步脚本会移除缺图标签，并对 train/val 执行“标签指向图片”和“图片具有标签”的双向检查。无法推断标签的孤立图片只会报错，不会被自动删除或猜测标注。被人工删除的 `crpd_white_*.jpg` 会记录到数据集根目录的 `crpd_white_rejected.txt`，后续再次运行 CRPD 警牌追加脚本时不会重新生成这些拒绝样本。

#### 特殊车牌 YOLO 框裁剪审阅集

`scripts/process_PPOCRv4_special_plates.py` 将 `datasets/特殊车牌/image/` 中的原图按照 `txt/` 下同名 YOLO 框裁剪，等比例缩放并边缘填充为 `128x48`。图片名和识别标签均直接使用车牌文件名。

```bash
python scripts/process_PPOCRv4_special_plates.py --dry-run
python scripts/process_PPOCRv4_special_plates.py --apply
```

默认只生成独立审阅集 `datasets/特殊车牌/PP-OCRv4_format/`，目录中包含 `images/车牌号.jpg` 和严格使用 TAB 的 `labels.txt`，不会自动追加到 `CBLPRD-330k`。若同一标签文件含多个框，仅当所有框 IoU 不低于 0.95 时才按重复标注合并；多个不同目标会直接报错。

审阅通过后使用下列命令，按学、港、澳、警四类分别做固定随机种子的 8:2 划分并追加：

```bash
python scripts/append_PPOCRv4_special_plates.py --dry-run
python scripts/append_PPOCRv4_special_plates.py --apply
```

当前审阅集共 328 张：学 86、港 81、澳 77、警 84。图片和标签一一对应，输出尺寸全部为 `128x48`；`粤ZZ473澳` 的两个高 IoU 重复框已合并为一个裁剪结果。2026-07-16 已使用随机种子 `20260716` 完成追加：train 新增 263 张（学 69、港 65、澳 62、警 67），val 新增 65 张（学 17、港 16、澳 15、警 17）。追加后 `train.txt` 为 275,363 行、`val.txt` 为 14,549 行，标签与图片全量双向检查均无缺失；独立审阅集仍完整保留。

#### CBLPRD basic/hard/special 分层

2026-07-17 使用`scripts/split_PPOCRv4_special_from_cblprd.py`，从`CBLPRD-330k/basic`和`hard`中识别包含`使、学、港、澳、警、领`的标签，按类别分别使用固定随机种子`20260717`重新做8:2划分，并移动到独立数据集：

```text
CBLPRD-330k/
├── basic/{train,val,train.txt,val.txt}
├── hard/{train,val,train.txt,val.txt}
└── special/
    ├── 使/{train,val,train.txt,val.txt}
    ├── 学/{train,val,train.txt,val.txt}
    ├── 港/{train,val,train.txt,val.txt}
    ├── 澳/{train,val,train.txt,val.txt}
    ├── 警/{train,val,train.txt,val.txt}
    └── 领/{train,val,train.txt,val.txt}
```

执行命令：

```bash
python scripts/split_PPOCRv4_special_from_cblprd.py
python scripts/split_PPOCRv4_special_from_cblprd.py --apply
```

迁移总数为25,937张：使5,659（train 4,527 / val 1,132）、学7,603（6,082 / 1,521）、港3,282（2,626 / 656）、澳3,272（2,618 / 654）、警156（125 / 31）、领5,965（4,772 / 1,193）。迁移后`basic`剩余198,758张（train 188,861 / val 9,897），`hard`剩余43,792张（train 41,532 / val 2,260）。三层合计仍为268,487张。

脚本默认只读预演，`--apply`执行前会检查源数据图片/标签双向一致、目标类别目录为空、标签不同时命中多个类别且目标文件名无冲突。源标签及完整迁移清单保存在`CBLPRD-330k/.special_migration_backups/20260717_205323/`；迁移报告为`special/split_report.json`。应用后已再次验证全部子数据集图片与标签一一对应，且`basic/hard`不再包含上述六类特殊字符。

2026-07-17 又使用`scripts/restore_PPOCRv4_police_from_audit.py`，从审计删除备份`20260717_173909`恢复61张警牌、从`20260717_203319`恢复1张警牌。恢复通过NTFS硬链接完成，不删除或改动原审计备份；62张恢复样本与原有156张警牌合并后，使用固定随机种子`20260717`重新划分为train 174张、val 44张，共218张。恢复后`special`共25,999张，`basic + hard + special`共268,549张。当前警牌图片和标签已双向校验一致，再次预演会识别为已恢复状态，不会重复追加。

```bash
python scripts/restore_PPOCRv4_police_from_audit.py
python scripts/restore_PPOCRv4_police_from_audit.py --apply
```

恢复前警牌标签、完整分组清单和恢复报告保存在`CBLPRD-330k/.special_migration_backups/20260717_211119_police_restore/`，当前报告为`special/police_restore_report.json`。

#### PP-OCR 48x160 预处理预览

`scripts/preview_ppocr_48x160.py` 按 RKNN C++ 识别示例的预处理顺序，将随机车牌图等比例缩放到高度 48、归一化到 `[-1, 1]`，再在右侧补浮点 0 到 `48x160`。脚本只复制随机原图并生成可视化预览，不修改训练集图片或标签。

```bash
conda activate YOLOv8n_LPRNet
python scripts/preview_ppocr_48x160.py --count 6 --seed 20260716
```

默认输出位于 `datasets/CBLPRD-330k/PP-OCRv4_48x160_preview/`，其中 `original/` 保存抽取的原图副本，`processed/` 保存将归一化张量还原为 BGR 后的 `48x160` PNG，`contact_sheet.png` 用于并排审阅，`manifest.tsv` 记录有效缩放宽度和右侧补零宽度。

#### RK3568 CBLPRD 完整验证包

2026-07-20 增加 `scripts/build_cblprd_board_eval_archive.py`：完整收集 basic、hard 及使、学、港、澳、警、领六类特殊验证集，并将全部 17,357 张图片和 8 份 UTF-8 manifest 直接写入 PAX `tar.gz`。解压后的顶层为 `CBLPRD/`，其内部保留 manifest 所引用的 `CBLPRD/basic|hard|special/...` 路径；共 17,365 个文件成员，可安全放入当前 RK3568 `/userdata` 的 inode 和容量余量内。

```bash
python scripts/build_cblprd_board_eval_archive.py --dry-run
python scripts/build_cblprd_board_eval_archive.py
```

默认输出为 `datasets/CBLPRD-330k/cblprd_eval.tar.gz`；已有同名文件时必须显式添加 `--overwrite`，避免误覆盖先前的固定评估集。

### 2. 训练 YOLOv8 车牌检测模型

```bash
cd 1_PC_Training/scripts
python train_yolo.py
```

说明：

- 默认入口脚本为 `train_yolo.py`
- `configs/yolo_config.yaml` 只描述当前特殊车牌派生数据集；
  `configs/yolo_config_baseline.yaml` 固定微调前原数据集。
- `configs/yolo_finetune_train.yaml` 独立维护训练超参数，默认使用 640 输入、
  batch 16、AdamW、`lr0=2e-4`、30 epochs、10 epochs patience、AMP 和余弦退火。
- 默认从旧训练 `weights/best.pt` 开始一次新微调，明确使用 `resume=false`；
  只有恢复本次中断的微调任务时才传
  `--resume-checkpoint path/to/last.pt`。
- 训练结果保存在
  `scripts/runs/finetune_results/yolov8n_special_other_20260726/`，
  训练后自动用 `best.pt` 评估 test split 并生成 `training_summary.json`。
- 当前数据约 6.2 GB、主机可用内存不足以安全做 RAM cache，训练配置固定
  `cache=false`。

训练前后可用相同入口保存逐类 val/test 指标；评估不设置高置信度门限，以保留
完整 PR 曲线：

```bash
python evaluate_yolo_finetune.py --split val --name before_finetune_val
python evaluate_yolo_finetune.py --split test --name before_finetune_test
python evaluate_yolo_finetune.py --model path/to/best.pt --split test --name after_finetune_test
```

2026-07-26 微调前固定基线（旧 `best.pt`，新派生集）：val 总体
mAP50/mAP50-95 为 0.9207/0.7718，`other` recall/AP50/AP50-95 为
0.5250/0.7151/0.5567；test 总体为 0.9099/0.7735，`other` 为
0.5105/0.7209/0.5812。训练后的模型必须同时比较 `other` 提升和
blue/green/yellow_single 三个核心类是否回退。

2026-07-26 微调完成：30 epochs，最佳为 epoch 23。新 `best.pt` 在固定 test
上的总体 mAP50/mAP50-95 为 0.9828/0.8520；`other`
recall/AP50/AP50-95 为 0.9189/0.9676/0.8601。blue、green、
yellow_single 的 AP50-95 相对旧模型变化为 +0.0053/-0.0007/+0.0305，
满足采用条件。完整数据审计、配置依据、训练过程、哈希和逐类对比见
`YOLO_FINETUNE_RECORD_20260726.md`。

#### YOLOv8-OBB 车牌数据集与训练

`scripts/build_yolo_obb_dataset.py` 从 CCPD2019 文件名和 CRPD 原始 txt
直接读取四角点，构建独立的 `datasets/yolo_obb_640/`。派生图片全部使用
同卷 NTFS 硬链接，数据目录由 `.gitignore` 忽略，可通过脚本重复构建：

```bash
cd 1_PC_Training
python scripts/build_yolo_obb_dataset.py --dry-run
python scripts/build_yolo_obb_dataset.py --apply
```

数据配置为 `configs/yolo_obb_config.yaml`，训练参数为
`configs/yolo_obb_train.yaml`。准备好本地 `yolov8n-obb.pt` 后，可继续使用
统一训练入口：

```bash
python scripts/train_yolo.py \
  --model path/to/yolov8n-obb.pt \
  --data configs/yolo_obb_config.yaml \
  --train-config configs/yolo_obb_train.yaml
```

训练配置使用数据集根目录下自动生成的 `val_balanced.txt` 进行验证。
该清单不重复图片、不使用 train/test 源图，常见三类各约 1,000 个框，
`other` 使用全部真实验证样本；完整 `images/val/` 仍保留用于按数据源复核。

OBB 数据的来源配额、CRPD `type=2` 过滤、`yellow_single` 总计 2 份、
`other` 总计 4 份、数据质量处理和全量硬链接验证结果见
`YOLO_OBB_DATASET_RECORD_20260731.md`。构建脚本会固定排除
已确认存在空标签或退化角点的 5 张 CRPD 异常源图，避免后续重建再次混入。

### 3. PaddleOCR 官方训练框架

`PaddleOCR/` 是从 PaddlePaddle/PaddleOCR 官方仓库直接克隆的独立训练工程，当前使用 `release/2.7` 分支、提交 `8cce9b6fd7ccb50226d0c38f94054d81c29b8184`。该目录保留自己的 `.git`，并已由项目根目录 `.gitignore` 整体忽略，官方源码和后续本地训练产物不会混入本工程提交。

车牌专用识别配置位于 `PaddleOCR/configs/rec/PP-OCRv4/ch_PP-OCRv4_rec_cblprd_48x160.yml`，使用 `PPLCNetV3 0.95 + SVTR + CTC/NRTR MultiHead`、73 字符专用字典和固定 `[3, 48, 160]` 数据加载尺寸。训练与验证均由 `RecResizeImg` 在内存中完成等比例缩放和右侧补零，不修改 `CBLPRD-330k` 原始图片。官方 `ch_PP-OCRv4_rec_train` Student 预训练权重保存在 `PaddleOCR/pretrain_models/ch_PP-OCRv4_rec_train/student.pdparams`，属于本地训练资源，不应提交到项目仓库。

### 4. 训练 LPRNet 字符识别模型

```bash
cd 1_PC_Training/LPRNet_Pytorch
python train_LPRNet.py
```

说明：

- 默认训练参数写在 `train_LPRNet.py` 中
- 当前脚本中包含本地路径默认值，若目录调整，需要同步修改
- 训练权重保存在 `LPRNet_Pytorch/weights/`

### 5. 测试 LPRNet 模型

```bash
cd 1_PC_Training/LPRNet_Pytorch
python test_LPRNet.py
```

## 注意事项

- 本目录中的训练脚本大量使用本地绝对路径默认值，迁移环境后应先检查路径参数。
- `datasets/`、训练输出、临时结果图不应作为常规源码改动提交。
- 若训练后需要上板，请将最终模型转交给 `2_Model_Conversion_PC_Simulation` 做 ONNX / RKNN 处理。
