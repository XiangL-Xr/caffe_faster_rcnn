python test_net.py  --gpu=1\
            --def=/home1/lixiang/Envs/workspace/py2.7_caffe_rcnn/py-faster-rcnn/models/pascal_voc/VGG16/faster_rcnn_end2end_2x/test.prototxt\
			--net=/home1/lixiang/Envs/workspace/py2.7_caffe_rcnn/py-faster-rcnn/output/faster_rcnn_end2end/voc_2007_trainval/vgg16_2x_prune_iter_100000.caffemodel\
			--cfg=/home1/lixiang/Envs/workspace/py2.7_caffe_rcnn/py-faster-rcnn/experiments/cfgs/faster_rcnn_end2end.yml\
			--imdb="voc_2007_test"
