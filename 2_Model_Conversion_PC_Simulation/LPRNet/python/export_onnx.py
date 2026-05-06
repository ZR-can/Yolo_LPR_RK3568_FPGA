import torch.nn as nn
import torch
import os
import sys
import urllib
import urllib.request
import time
import traceback
import numpy as np
import onnx
CHARS = ['京', '沪', '津', '渝', '冀', '晋', '蒙', '辽', '吉', '黑',
         '苏', '浙', '皖', '闽', '赣', '鲁', '豫', '鄂', '湘', '粤',
         '桂', '琼', '川', '贵', '云', '藏', '陕', '甘', '青', '宁',
         '新', '港', '澳', '警', '学', '领', '使', 
         '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
         'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K',
         'L', 'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
         'W', 'X', 'Y', 'Z', 'I', 'O', '-'
         ]
MODEL_DIR = '../model/'
MODEL_PATH = MODEL_DIR + 'LPRNet_7char_20260425124555.pth'
#MODEL_PATH = MODEL_DIR + 'LPRNet_8char_20260425152745.pth'
# 定义ONNX输出路径（统一文件名）
ONNX_SAVE_PATH = MODEL_DIR + 'lprnet7.onnx'
#ONNX_SAVE_PATH = MODEL_DIR + 'lprnet8.onnx'  # 若用8字符模型则启用此行

# python export_onnx.py

# Convert maxpool3d to the class of maxpool2d
class maxpool_3d(nn.Module):
    def __init__(self, kernel_size, stride):
        super(maxpool_3d, self).__init__()
        assert(len(kernel_size)==3 and len(stride)==3)
        kernel_size2d1 = kernel_size[-2:]
        stride2d1 = stride[-2:]
        kernel_size2d2 = (kernel_size[0],kernel_size[0])
        stride2d2 = (kernel_size[0], stride[0])
        self.maxpool1 = nn.MaxPool2d(kernel_size=kernel_size2d1, stride=stride2d1)
        self.maxpool2 = nn.MaxPool2d(kernel_size=kernel_size2d2, stride=stride2d2)
    def forward(self,x):
        x = self.maxpool1(x)
        x = x.transpose(1,3)
        x = self.maxpool2(x)
        x = x.transpose(1,3)
        return x 


class small_basic_block(nn.Module):
    def __init__(self, ch_in, ch_out):
        super(small_basic_block, self).__init__()
        self.block = nn.Sequential(
            nn.Conv2d(ch_in, ch_out // 4, kernel_size=1),
            nn.ReLU(),
            nn.Conv2d(ch_out // 4, ch_out // 4, kernel_size=(3, 1), padding=(1, 0)),
            nn.ReLU(),
            nn.Conv2d(ch_out // 4, ch_out // 4, kernel_size=(1, 3), padding=(0, 1)),
            nn.ReLU(),
            nn.Conv2d(ch_out // 4, ch_out, kernel_size=1),
        )
    def forward(self, x):
        return self.block(x)

class LPRNet(nn.Module):
    def __init__(self, class_num, dropout_rate):
        super(LPRNet, self).__init__()
        self.class_num = class_num
        self.backbone = nn.Sequential(
            nn.Conv2d(in_channels=3, out_channels=64, kernel_size=3, stride=1), # 0
            nn.BatchNorm2d(num_features=64),
            nn.ReLU(),  # 2
            maxpool_3d(kernel_size=(1, 3, 3), stride=(1, 1, 1)),
            small_basic_block(ch_in=64, ch_out=128),    # *** 4 ***
            nn.BatchNorm2d(num_features=128),
            nn.ReLU(),  # 6
            maxpool_3d(kernel_size=(1, 3, 3), stride=(2, 1, 2)),
            small_basic_block(ch_in=64, ch_out=256),   # 8
            nn.BatchNorm2d(num_features=256),
            nn.ReLU(),  # 10
            small_basic_block(ch_in=256, ch_out=256),   # *** 11 ***
            nn.BatchNorm2d(num_features=256),   # 12
            nn.ReLU(),
            maxpool_3d(kernel_size=(1, 3, 3), stride=(4, 1, 2)),  # 14
            nn.Dropout(dropout_rate),
            nn.Conv2d(in_channels=64, out_channels=256, kernel_size=(1, 4), stride=1),  # 16
            nn.BatchNorm2d(num_features=256),
            nn.ReLU(),  # 18
            nn.Dropout(dropout_rate),
            nn.Conv2d(in_channels=256, out_channels=class_num, kernel_size=(13, 1), stride=1), # 20
            nn.BatchNorm2d(num_features=class_num),
            nn.ReLU(),  # *** 22 ***
        )
        self.container = nn.Sequential(
            nn.Conv2d(in_channels=256+class_num+128+64, out_channels=self.class_num, kernel_size=(1,1), stride=(1,1)),
        )

    def forward(self, x):
        keep_features = list()
        for i, layer in enumerate(self.backbone.children()):
            x = layer(x)
            if i in [2, 6, 13, 22]: 
                keep_features.append(x)

        global_context = list()
        for i, f in enumerate(keep_features):
            if i in [0, 1]:
                f = nn.AvgPool2d(kernel_size=5, stride=5)(f)
            if i in [2]:
                f = nn.AvgPool2d(kernel_size=(4, 10), stride=(4, 2))(f)
            f_pow = torch.pow(f, 2)
            f_mean = torch.mean(f_pow)
            f = torch.div(f, f_mean)
            global_context.append(f)

        x = torch.cat(global_context, 1)
        x = self.container(x)
        logits = torch.mean(x, dim=2)

        return logits

def readable_speed(speed):
    speed_bytes = float(speed)
    speed_kbytes = speed_bytes / 1024
    if speed_kbytes > 1024:
        speed_mbytes = speed_kbytes / 1024
        if speed_mbytes > 1024:
            speed_gbytes = speed_mbytes / 1024
            return "{:.2f} GB/s".format(speed_gbytes)
        else:
            return "{:.2f} MB/s".format(speed_mbytes)
    else:
        return "{:.2f} KB/s".format(speed_kbytes)

def show_progress(blocknum, blocksize, totalsize):
    speed = (blocknum * blocksize) / (time.time() - start_time)
    speed_str = " Speed: {}".format(readable_speed(speed))
    recv_size = blocknum * blocksize

    f = sys.stdout
    progress = (recv_size / totalsize)
    progress_str = "{:.2f}%".format(progress * 100)
    n = round(progress * 50)
    s = ('#' * n).ljust(50, '-')
    f.write(progress_str.ljust(8, ' ') + '[' + s + ']' + speed_str)
    f.flush()
    f.write('\r\n')

def check_and_download_origin_model():
    global start_time
    if not os.path.exists(MODEL_PATH):
        print('--> Download {}'.format(MODEL_PATH))
        url = 'https://github.com/sirius-ai/LPRNet_Pytorch/raw/master/weights/Final_LPRNet_model.pth'
        download_file = MODEL_PATH
        try:
            start_time = time.time()
            urllib.request.urlretrieve(url, download_file, show_progress)
        except:
            print('Download {} failed.'.format(download_file))
            print(traceback.format_exc())
            exit(-1)
        print('done')

if __name__ == "__main__":
    
    # Download model if not exist (from https://github.com/sirius-ai/LPRNet_Pytorch/blob/master/weights)
    check_and_download_origin_model()

    device = torch.device('cpu')
    lprnet = LPRNet(class_num=len(CHARS), dropout_rate=0).to(device)
    lprnet.load_state_dict(torch.load(MODEL_PATH, map_location=torch.device('cpu'),weights_only=True))
    lprnet.eval()


    torch.onnx.export(lprnet,              
                  torch.randn(1,3,24,94),                         
                  ONNX_SAVE_PATH,   
                  export_params=True,        
                  input_names = ['input'],   
                  output_names = ['output'],
                  #opset_version=19
                 )

    # 检查文件是否存在（匹配实际导出的文件名）
    if os.path.exists(ONNX_SAVE_PATH):
        print(f'ONNX模型已保存至: {ONNX_SAVE_PATH}')
    else:
        print(f'导出ONNX失败：文件 {ONNX_SAVE_PATH} 未生成')

    model = onnx.load(ONNX_SAVE_PATH)
    print("\n===== ONNX 模型信息 =====")
    print("ir_version    :", model.ir_version)
    print("opset 版本    :", model.opset_import[0].version)
    print("========================\n")

