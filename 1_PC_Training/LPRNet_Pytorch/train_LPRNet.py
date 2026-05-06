# -*- coding: utf-8 -*-
# /usr/bin/env/python3

'''
Pytorch implementation for LPRNet.
Author: aiboy.wei@outlook.com .
'''

from data.load_data import CHARS, CHARS_DICT, LPRDataLoader
from model.LPRNet import build_lprnet
# import torch.backends.cudnn as cudnn
from torch.autograd import Variable
import torch.nn.functional as F
from torch.utils.data import *
from torch import optim
import torch.nn as nn
import numpy as np
import argparse
import torch
import time
import os

def sparse_tuple_for_ctc(T_length, lengths):
    input_lengths = []
    target_lengths = []

    for ch in lengths:
        input_lengths.append(T_length)
        target_lengths.append(ch)

    return tuple(input_lengths), tuple(target_lengths)

def adjust_learning_rate(optimizer, cur_epoch, base_lr, lr_schedule):
    """
    Sets the learning rate
    """
    lr = 0
    for i, e in enumerate(lr_schedule):
        if cur_epoch < e:
            lr = base_lr * (0.1 ** i)
            break
    if lr == 0:
        lr = base_lr
    for param_group in optimizer.param_groups:
        param_group['lr'] = lr

    return lr

def get_parser():
    parser = argparse.ArgumentParser(description='parameters to train net')
    parser.add_argument('--img_size', default=[94, 24], help='the image size')

    parser.add_argument('--max_epoch', default=5, help='epoch to train the network,use ADAM')
    #parser.add_argument('--max_epoch', default=10, help='epoch to train the network,use ADAM')
    #parser.add_argument('--max_epoch', default=15, help='epoch to train the network,use RMSProp')
    
    #parser.add_argument('--train_img_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_format\train_lprnet", help='the train images path')
        
    #parser.add_argument('--old_train_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_7char\lprnet_7char_tight_train", help='the old train images path')
    #parser.add_argument('--new_train_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\yolo_lprnet_crops\blue\train", help='the new YOLO train images path')
    #parser.add_argument('--new_ratio', default=0.2, type=float, help='ratio of new data in each batch')

    parser.add_argument('--old_train_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_8char\train", help='the old train images path')
    parser.add_argument('--new_train_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\yolo_lprnet_crops\green\train", help='the new YOLO train images path')
    parser.add_argument('--new_ratio', default=0.18, type=float, help='ratio of new data in each batch')

    #parser.add_argument('--test_img_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_7char\lprnet_7char_tight_val", help='the test images path')
    parser.add_argument('--test_img_dirs', default=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_8char\lprnet_8char_tight_val", help='the test images path')

    #parser.add_argument('--learning_rate', default=0.1, help='base value of learning rate.')
    #parser.add_argument('--learning_rate', default=0.01, help='base value of learning rate.')
    parser.add_argument('--learning_rate', default=0.0001, help='base value of learning rate.')
    #parser.add_argument('--lr_schedule', default=[4, 8, 12, 14, 16], help='schedule for learning rate.')
    #parser.add_argument('--lr_schedule', default=[2, 4, 6, 8, 12], help='schedule for learning rate.')
    parser.add_argument('--lr_schedule', default=[2, 4, 6], help='schedule for learning rate.')    

    parser.add_argument('--dropout_rate', default=0.5, help='dropout rate.')     
    parser.add_argument('--lpr_max_len', default=8, help='license plate number max length.')
    parser.add_argument('--train_batch_size', default=256, help='training batch size.')
    parser.add_argument('--test_batch_size', default=256, help='testing batch size.')
    parser.add_argument('--phase_train', default=True, type=bool, help='train or test phase flag.')
    parser.add_argument('--num_workers', default=8, type=int, help='Number of workers used in dataloading')
    parser.add_argument('--cuda', default=True, type=bool, help='Use cuda to train model')
    parser.add_argument('--resume_epoch', default=0, type=int, help='resume iter for retraining')
    parser.add_argument('--save_interval', default=2000, type=int, help='interval for save model state dict')
    parser.add_argument('--test_interval', default=2000, type=int, help='interval for evaluate')
    parser.add_argument('--momentum', default=0.9, type=float, help='momentum')
    #parser.add_argument('--weight_decay', default=1e-4, type=float, help='Weight decay for SGD')
    parser.add_argument('--weight_decay', default=2e-5, type=float, help='Weight decay for SGD')

    #parser.add_argument('--save_folder', default='./weights/7char/', help='Location to save checkpoint models')
    parser.add_argument('--save_folder', default='./weights/8char/', help='Location to save checkpoint models')

    #parser.add_argument('--pretrained_model', default='', help='pretrained base model')
    parser.add_argument('--pretrained_model', default=r'D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\LPRNet_Pytorch\weights\8char\LPRNet_8char_20260425150614.pth', help='pretrained base model')

    args = parser.parse_args()

    return args

def collate_fn(batch):
    imgs = []
    labels = []
    lengths = []
    for _, sample in enumerate(batch):
        img, label, length = sample
        imgs.append(torch.from_numpy(img))
        labels.extend(label)  
        lengths.append(length)
    labels = np.asarray(labels).flatten().astype(int)

    return (torch.stack(imgs, 0), torch.from_numpy(labels), lengths)

def train():
    args = get_parser()

    T_length = 18 # args.lpr_max_len
    epoch = 0 + args.resume_epoch
    loss_val = 0

    if not os.path.exists(args.save_folder):
        os.mkdir(args.save_folder)

    lprnet = build_lprnet(lpr_max_len=args.lpr_max_len, phase=args.phase_train, class_num=len(CHARS), dropout_rate=args.dropout_rate)
    device = torch.device("cuda:0" if args.cuda else "cpu")
    print("Device: ", device)
    lprnet.to(device)
    print("Successful to build network!")

    # load pretrained model
    if args.pretrained_model:
        lprnet.load_state_dict(torch.load(args.pretrained_model))
        print("load pretrained model successful!")
    else:
        def xavier(param):
            nn.init.xavier_uniform(param)

        def weights_init(m):
            for key in m.state_dict():
                if key.split('.')[-1] == 'weight':
                    if 'conv' in key:
                        nn.init.kaiming_normal_(m.state_dict()[key], mode='fan_out')
                    if 'bn' in key:
                        m.state_dict()[key][...] = xavier(1)
                elif key.split('.')[-1] == 'bias':
                    m.state_dict()[key][...] = 0.01

        lprnet.backbone.apply(weights_init)
        lprnet.container.apply(weights_init)
        print("initial net weights successful!")

    # define optimizer
    # optimizer = optim.SGD(lprnet.parameters(), lr=args.learning_rate,
    #                       momentum=args.momentum, weight_decay=args.weight_decay)
    '''
    optimizer = optim.RMSprop(lprnet.parameters(), lr=args.learning_rate, alpha = 0.9, eps=1e-08,
                         momentum=args.momentum, weight_decay=args.weight_decay)
    '''                     
    optimizer = optim.AdamW(lprnet.parameters(), 
                           lr=args.learning_rate, 
                            #betas=(0.9, 0.999), 
                            #eps=1e-08,
                            weight_decay=args.weight_decay) 
    
    #train_img_dirs = os.path.expanduser(args.train_img_dirs)
    #test_img_dirs = os.path.expanduser(args.test_img_dirs)
    #rain_dataset = LPRDataLoader(train_img_dirs.split(','), args.img_size, args.lpr_max_len)
    #test_dataset = LPRDataLoader(test_img_dirs.split(','), args.img_size, args.lpr_max_len)
    
    # ================= 修改的数据集加载区 =================
    # 解析训练集（旧数据与新数据）
    old_train_dirs = os.path.expanduser(args.old_train_dirs).split(',')
    new_train_dirs = os.path.expanduser(args.new_train_dirs).split(',')
    print('旧训练集路径列表:', old_train_dirs)
    print('新训练集路径列表:', new_train_dirs)
    train_dataset = LPRDataLoader(
        old_img_dirs=old_train_dirs, 
        new_img_dirs=new_train_dirs, 
        imgSize=args.img_size, 
        lpr_max_len=args.lpr_max_len,
        new_ratio=args.new_ratio  # 控制新数据占比
    )
    
    # 解析测试集（测试集不需要过采样扩充，new_img_dirs 传空列表，new_ratio 传 0.0）
    test_img_dirs = os.path.expanduser(args.test_img_dirs).split(',')
    print('测试集路径列表:', test_img_dirs)
    test_dataset = LPRDataLoader(
        old_img_dirs=test_img_dirs, 
        new_img_dirs=[], 
        imgSize=args.img_size, 
        lpr_max_len=args.lpr_max_len,
        new_ratio=0.0
    )
    # ======================================================
    epoch_size = len(train_dataset) // args.train_batch_size
    max_iter = args.max_epoch * epoch_size

    ctc_loss = nn.CTCLoss(blank=len(CHARS)-1, reduction='mean') # reduction: 'none' | 'mean' | 'sum'

    if args.resume_epoch > 0:
        start_iter = args.resume_epoch * epoch_size
    else:
        start_iter = 0

    for iteration in range(start_iter, max_iter):
        if iteration % epoch_size == 0:
            # create batch iterator
            batch_iterator = iter(DataLoader(train_dataset, args.train_batch_size, shuffle=True, num_workers=args.num_workers, collate_fn=collate_fn))
            loss_val = 0
            epoch += 1

        if iteration !=0 and iteration % args.save_interval == 0:
            torch.save(lprnet.state_dict(), args.save_folder + 'LPRNet_' + '_iteration_' + repr(iteration) + '.pth')

        if (iteration + 1) % args.test_interval == 0:
            Greedy_Decode_Eval(lprnet, test_dataset, args)
            # lprnet.train() # should be switch to train mode

        start_time = time.time()
        # load train data
        images, labels, lengths = next(batch_iterator)
        # labels = np.array([el.numpy() for el in labels]).T
        # print(labels)
        # get ctc parameters
        input_lengths, target_lengths = sparse_tuple_for_ctc(T_length, lengths)
        # update lr
        lr = adjust_learning_rate(optimizer, epoch, args.learning_rate, args.lr_schedule)

        if args.cuda:
            images = Variable(images, requires_grad=False).cuda()
            labels = Variable(labels, requires_grad=False).cuda()
        else:
            images = Variable(images, requires_grad=False)
            labels = Variable(labels, requires_grad=False)

        # forward
        logits = lprnet(images)
        log_probs = logits.permute(2, 0, 1) # for ctc loss: T x N x C
        # print(labels.shape)
        log_probs = log_probs.log_softmax(2).requires_grad_()
        # log_probs = log_probs.detach().requires_grad_()
        # print(log_probs.shape)
        # backprop
        optimizer.zero_grad()
        loss = ctc_loss(log_probs, labels, input_lengths=input_lengths, target_lengths=target_lengths)
        if loss.item() == np.inf:
            continue
        loss.backward()
        optimizer.step()
        loss_val += loss.item()
        end_time = time.time()
        if iteration % 20 == 0:
            print('Epoch:' + repr(epoch) + ' || epochiter: ' + repr(iteration % epoch_size) + '/' + repr(epoch_size)
                  + '|| Totel iter ' + repr(iteration) + ' || Loss: %.4f||' % (loss.item()) +
                  'Batch time: %.4f sec. ||' % (end_time - start_time) + 'LR: %.8f' % (lr))
    # final test
    print("Final test Accuracy:")
    Greedy_Decode_Eval(lprnet, test_dataset, args)

    # 保存模型，时间戳
    timestamp = time.strftime("%Y%m%d%H%M%S", time.localtime())

    #torch.save(lprnet.state_dict(), args.save_folder + 'LPRNet_7char_' + timestamp + '.pth')
    #print('成功保存模型！ 模型文件名：LPRNet_7char_' + timestamp + '.pth')

    torch.save(lprnet.state_dict(), args.save_folder + 'LPRNet_8char_' + timestamp + '.pth')
    print('成功保存模型！ 模型文件名：LPRNet_8char_' + timestamp + '.pth')


def Greedy_Decode_Eval(Net, datasets, args):
    # TestNet = Net.eval()
    epoch_size = len(datasets) // args.test_batch_size
    batch_iterator = iter(DataLoader(datasets, args.test_batch_size, shuffle=True, num_workers=args.num_workers, collate_fn=collate_fn))

    Tp = 0 # 完全识别正确的车牌数
    Tn_1 = 0 # 识别长度不匹配的车牌数
    Tn_2 = 0 # 识别长度匹配但内容错误的车牌数
    t1 = time.time()
    for i in range(epoch_size):
        # load train data
        images, labels, lengths = next(batch_iterator)
        start = 0
        targets = []
        for length in lengths:
            label = labels[start:start+length]
            targets.append(label)
            start += length
        targets = np.array([el.numpy() for el in targets], dtype=object)

        if args.cuda:
            images = Variable(images.cuda())
        else:
            images = Variable(images)

        # forward
        prebs = Net(images)
        # greedy decode
        prebs = prebs.cpu().detach().numpy()
        preb_labels = list()
        for i in range(prebs.shape[0]):
            preb = prebs[i, :, :]
            preb_label = list()
            for j in range(preb.shape[1]):
                preb_label.append(np.argmax(preb[:, j], axis=0))
            no_repeat_blank_label = list()
            pre_c = preb_label[0]
            if pre_c != len(CHARS) - 1:
                no_repeat_blank_label.append(pre_c)
            for c in preb_label: # dropout repeate label and blank label
                if (pre_c == c) or (c == len(CHARS) - 1):
                    if c == len(CHARS) - 1:
                        pre_c = c
                    continue
                no_repeat_blank_label.append(c)
                pre_c = c
            preb_labels.append(no_repeat_blank_label)
        for i, label in enumerate(preb_labels):
            if len(label) != len(targets[i]):
                Tn_1 += 1
                continue
            if (np.asarray(targets[i]) == np.asarray(label)).all():
                Tp += 1
            else:
                Tn_2 += 1

    total = Tp + Tn_1 + Tn_2
    if total == 0:
        Acc = 0.0
        print("[Warn] No samples were evaluated (total=0). Accuracy is set to 0.0.")
    else:
        Acc = Tp * 1.0 / total
    print("[Info] Test Accuracy: {} [{}:{}:{}:{}]".format(Acc, Tp, Tn_1, Tn_2, total))
    t2 = time.time()
    print("[Info] Test Speed: {}s 1/{}]".format((t2 - t1) / len(datasets), len(datasets)))


if __name__ == "__main__":
    train()
