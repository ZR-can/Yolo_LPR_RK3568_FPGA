import os
import cv2
import numpy as np
import argparse
import random
from rknn.api import RKNN
from torch.utils.data import Dataset, DataLoader
from imutils import paths
# python lprnet_PC_eval.py --onnx_path ../model/lprnet8.onnx
# ==========================================
# 1. 字符集定义
# ==========================================
CHARS = ['京', '沪', '津', '渝', '冀', '晋', '蒙', '辽', '吉', '黑',
         '苏', '浙', '皖', '闽', '赣', '鲁', '豫', '鄂', '湘', '粤',
         '桂', '琼', '川', '贵', '云', '藏', '陕', '甘', '青', '宁',
         '新', '港', '澳', '警', '学', '领', '使', 
         '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
         'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K',
         'L', 'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
         'W', 'X', 'Y', 'Z','O', 'I'
         ]

CHARS_DICT = {char:i for i, char in enumerate(CHARS)}

# ==========================================
# 2. 数据加载类
# ==========================================
class LPRDataLoader(Dataset):
    def __init__(self, img_dir, imgSize, lpr_max_len, PreprocFun=None):
        self.img_dir = img_dir
        self.img_paths = []
        for i in range(len(img_dir)):
            self.img_paths += [el for el in paths.list_images(img_dir[i])]
            
        random.shuffle(self.img_paths)
        self.img_size = imgSize
        self.lpr_max_len = lpr_max_len
        # 如果没有提供预处理函数，默认返回原始 HWC 图像供 RKNN 使用
        self.PreprocFun = PreprocFun if PreprocFun is not None else (lambda x: x)

    def __len__(self):
        return len(self.img_paths)

    def __getitem__(self, index):
        filename = os.path.normpath(self.img_paths[index])
        
        try:
            # 支持中文路径的读取方式
            img_data = np.fromfile(filename, dtype=np.uint8)
            image = cv2.imdecode(img_data, cv2.IMREAD_COLOR)
            
            if image is None:
                image = np.zeros((self.img_size[1], self.img_size[0], 3), dtype=np.uint8)
            else:
                image = cv2.resize(image, (self.img_size[0], self.img_size[1]))
        except Exception as e:
            print(f"读取图片出错: {filename}, {e}")
            image = np.zeros((self.img_size[1], self.img_size[0], 3), dtype=np.uint8)
        
        # 处理标签
        basename = os.path.basename(filename)
        imgname, _ = os.path.splitext(basename)
        imgname = imgname.split("-")[0].split("_")[0]
        
        label = []
        for c in imgname:
            if c in CHARS_DICT:
                label.append(CHARS_DICT[c])
        
        return self.PreprocFun(image), label, len(label)


# ==========================================
# 3. 推理辅助函数
# ==========================================
def decode_logic(preds, chars):
    """
    贪心解码逻辑，去除重复和空白字符
    """
    pred_labels = []
    for i in range(preds.shape[0]):
        pred = preds[i, :, :]
        raw_label = []
        for j in range(pred.shape[1]):
            raw_label.append(np.argmax(pred[:, j], axis=0))
        
        no_repeat_blank_label = []
        pre_c = raw_label[0]
        if pre_c != len(chars) - 1:
            no_repeat_blank_label.append(pre_c)
        for c in raw_label:
            if (pre_c == c) or (c == len(chars) - 1):
                if c == len(chars) - 1:
                    pre_c = c
                continue
            no_repeat_blank_label.append(c)
            pre_c = c
        pred_labels.append(no_repeat_blank_label)
    return pred_labels

# ==========================================
# 4. 主程序
# ==========================================
def main():
    parser = argparse.ArgumentParser(description='LPRNet RKNN 集成测试')
    parser.add_argument('--onnx_path', type=str, required=True, help='ONNX模型路径')
    parser.add_argument('--test_img_dirs', default="../data/test", help='测试集路径')
    parser.add_argument('--dataset_txt', default='../model/dataset.txt', help='量化校准数据集列表')
    parser.add_argument('--quant',action='store_true', help='是否开启i8量化')
    args = parser.parse_args()

    # --- RKNN 配置 ---
    rknn = RKNN(verbose=False)
    print('--> 配置模型')
    # mean/std 对应 (x - 127.5) / 128.0 的预处理方式
    rknn.config(
        target_platform='rk3568',
        mean_values=[[127.5, 127.5, 127.5]],
        std_values=[[128.0, 128.0, 128.0]]
    )

    # --- 模型加载与构建 ---
    print('--> 加载 ONNX 模型')
    if rknn.load_onnx(model=args.onnx_path) != 0:
        print('加载失败'); return

    print('--> 构建 RKNN 模型')
    if args.quant: print('开启 i8 量化')
    ret = rknn.build(do_quantization=args.quant, dataset=args.dataset_txt)
    if ret != 0:
        print('构建失败'); return

    # --- 初始化运行时 (PC 模拟器) ---
    print('--> 初始化 PC 模拟器')
    ret = rknn.init_runtime(
        target=None        
        # target=None 使用PC模拟器,target 设为 None 时，需要先调用 build 或 hybrid_quantization 接口才可让模型在模拟器上运行。
        # perf_debug=False,      # 开启性能评估
        # eval_mem=True         # 开启内存评估
    )
    if ret != 0:
        print('Init runtime failed!')
        exit(ret)
    print('done')

    # --- 数据准备 ---
    test_dirs = os.path.expanduser(args.test_img_dirs).split(',')
    # 使用默认 PreprocFun (lambda x: x) 以保持 HWC 格式供 RKNN 推理
    test_dataset = LPRDataLoader(test_dirs, [94, 24], 8)
    test_loader = DataLoader(test_dataset, batch_size=1, shuffle=False)

    # --- 评估循环 ---
    tp, tn1, tn2 = 0, 0, 0
    print(f"--> 开始评估，样本总数: {len(test_dataset)}")

    for i, (img, target, length) in enumerate(test_loader):
        # 1. 确保数据维度正确：从 [1, 24, 94, 3] 确保是 numpy 数组
        # DataLoader batch_size=1 时，img 已经是 4 维的 Tensor
        img_np = img.numpy().astype(np.uint8)
        
        # 2. 执行推理时明确指定 data_format
        # 传入 inputs=[img_np]，此时 img_np 形状应为 (1, 24, 94, 3)
        outputs = rknn.inference(inputs=[img_np], data_format='nhwc')
        
        # 3. 解码与比对
        preds = decode_logic(outputs[0], CHARS)
        pred_label = preds[0]
        gt_label = [int(x) for x in target]
        
        # 统计逻辑保持不变
        if len(pred_label) != len(gt_label):
            tn1 += 1
        elif np.array_equal(pred_label, gt_label):
            tp += 1
        else:
            tn2 += 1

        if i % 100 == 0 and i > 0:
            print(f"进度: {i}/{len(test_dataset)}")

    # --- 结果汇总 ---
    total = tp + tn1 + tn2
    acc = tp / total if total > 0 else 0
    print(f"\n[测试报告]")
    print(f"准确率 (Acc): {acc:.4f}")
    print(f"统计 [正确: {tp}, 长度错误: {tn1}, 内容错误: {tn2}, 总计: {total}]")

    rknn.release()

if __name__ == '__main__':
    main()