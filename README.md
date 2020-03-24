# AVS/caffe_IncReg(py-faster-rcnn)

AITISA工作组Caffe平台基准代码(检测部分)

## 简介
### 本仓库为增量正则化剪枝算法在检测任务(py—faster-rcnn)上的代码实现。

## 依赖环境
	- ubuntu == 1604
    - python ==  2.7
	- cuda   ==  9.0
    - use cudnn    

## 配置与安装

### Requirements
**NOTE** If you are having issues compiling and you are using a recent version of CUDA/cuDNN, please consult [this issue](https://github.com/rbgirshick/py-faster-rcnn/issues/509?_pjax=%23js-repo-pjax-container#issuecomment-284133868) for a workaround

1. Requirements for `Caffe` and `pycaffe` (see: [Caffe installation instructions](http://caffe.berkeleyvision.org/installation.html))

  **Note:** Caffe *must* be built with support for Python layers!

  ```make
  # In your Makefile.config, make sure to have this line uncommented
  WITH_PYTHON_LAYER := 1
  # Unrelatedly, it's also recommended that you use CUDNN
  USE_CUDNN := 1
  ```

2. Python packages you might not have: `cython`, `python-opencv`, `easydict`

### Installation

1. Clone the Faster R-CNN repository

2. We'll call the directory that you cloned Faster R-CNN into `FRCN_ROOT`

3. Build the Cython modules
    ```Shell
    cd $FRCN_ROOT/lib
    make
    ```

4. Build Caffe and pycaffe
    ```Shell
    cd $FRCN_ROOT/caffe-fast-rcnn
    # Now follow the Caffe installation instructions here:
    #   http://caffe.berkeleyvision.org/installation.html

    # If you're experienced with Caffe and have all of the requirements installed
    # and your Makefile.config in place, then simply do:
    make -j8 && make pycaffe
    ```

### Data Preparation

1. Download the training, validation, test data and VOCdevkit

	```Shell
	wget http://host.robots.ox.ac.uk/pascal/VOC/voc2007/VOCtrainval_06-Nov-2007.tar
	wget http://host.robots.ox.ac.uk/pascal/VOC/voc2007/VOCtest_06-Nov-2007.tar
	wget http://host.robots.ox.ac.uk/pascal/VOC/voc2007/VOCdevkit_08-Jun-2007.tar
	```

2. Extract all of these tars into one directory named `VOCdevkit`

	```Shell
	tar xvf VOCtrainval_06-Nov-2007.tar
	tar xvf VOCtest_06-Nov-2007.tar
	tar xvf VOCdevkit_08-Jun-2007.tar
	```

3. It should have this basic structure

	```Shell
  	$VOCdevkit/                           # development kit
  	$VOCdevkit/VOCcode/                   # VOC utility code
  	$VOCdevkit/VOC2007                    # image sets, annotations, etc.
  	# ... and several other directories ...
  	```

4. Create symlinks for the PASCAL VOC dataset

	```Shell
    cd $FRCN_ROOT/data
    ln -s $VOCdevkit VOCdevkit2007
    ```
    Using symlinks is a good idea because you will likely want to share the same PASCAL dataset installation between multiple projects.
5. [Optional] follow similar steps to get PASCAL VOC 2010 and 2012
6. [Optional] If you want to use COCO, please see some notes under `data/README.md`

### Download pre-trained ImageNet models

Pre-trained ImageNet models can be downloaded for the three networks described in the paper: ZF and VGG16.

```Shell
cd $FRCN_ROOT
./data/scripts/fetch_imagenet_models.sh
```
VGG16 comes from the [Caffe Model Zoo](https://github.com/BVLC/caffe/wiki/Model-Zoo), but is provided here for your convenience.
ZF was trained at MSRA.

## 用法

To train and test a Faster R-CNN detector use `experiments/scripts/faster_rcnn_end2end.sh`.
Output log is saved in `$FRCN_ROOT/vgg16_prune_logs`.
Output model is written underneath `$FRCN_ROOT/output`.

### pruning and retraining

```Shell
cd $FRCN_ROOT
nohup ./experiments/scripts/faster_rcnn_end2end.sh [GPU_ID] [NET] [--set ...] > /dev/null &
# GPU_ID is the GPU you want to train on
# NET in {ZF, VGG_CNN_M_1024, VGG16} is the network arch to use
# --set ... allows you to specify fast_rcnn.config options, e.g.
#   --set EXP_DIR seed_rng1701 RNG_SEED 1701
```

Demo(prune_ratio = 0.5)

```Shell
cd $FRCN_ROOT
nohup ./experiments/scripts/vgg16_2x_prune.sh 0 VGG16 pascal_voc > /dev/null &
```

### Check the log
1. There are two logs generated during pruning: `log_<TimeID>_acc.txt` and `log_<TimeID>_prune.txt`. The former saves the logs printed by the original Caffe; the latter saves the logs printed by our added codes.
2. Go to the logs saved folder, e.g., `$FRCN_ROOT/vgg16_prune_logs` for vgg16, then run `cat *prune.txt | grep app` you will see the pruning and retraining course.
### testing

```Shell
cd $FRCN_ROOT/tools
sh test_2.0x_v07.sh
```

## 模型精度

VGG16

network |  mAP  | prune ratio | Speedup ratio
--------|---------------|---------------|----------------
VGG16_BASE  |  0.6946  |  0.00  |  1.0x  | 
VGG16_PRUNE  |  0.6792  |  0.50  |  2.0x  |

