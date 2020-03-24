#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <ctime>
#include "caffe/sgd_solvers.hpp"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/upgrade_proto.hpp"
#include "caffe/util/math_functions.hpp"

#include <boost/thread.hpp>
#include <numeric>
#include "caffe/layer.hpp"
#include "caffe/adaptive_probabilistic_pruning.hpp"

namespace caffe {

template <typename Dtype>
void Layer<Dtype>::InitMutex() {
  forward_mutex_.reset(new boost::mutex());
}

template <typename Dtype>
void Layer<Dtype>::Lock() {
  if (IsShared()) {
    forward_mutex_->lock();
  }
}

template <typename Dtype>
void Layer<Dtype>::Unlock() {
  if (IsShared()) {
    forward_mutex_->unlock();
  }
}

/// @lixiang, for pruning
template<typename Dtype>
void Layer<Dtype>::IF_layer_prune_finished() {
    const string layer_name = this->layer_param_.name();
    if (APP<Dtype>::layer_index.count(layer_name) != 0) {
        const int L = APP<Dtype>::layer_index[layer_name];
        if (APP<Dtype>::iter_prune_finished[L] == INT_MAX) {
            const bool layer_finish = APP<Dtype>::pruned_ratio_for_comparison[L] >= APP<Dtype>::current_prune_ratio[L]; // layer pruning target achieved
            const bool net_finish_speed = APP<Dtype>::IF_speedup_achieved;      // net pruning target of speed achieved
            const bool net_finish_param = APP<Dtype>::IF_compRatio_achieved;     // net pruning target of compression achieved

            if (layer_finish || net_finish_speed || net_finish_param) {
                APP<Dtype>::iter_prune_finished[L] = APP<Dtype>::step_ - 1;
                // print when finished
                char rlayer[10], rrow[10], rcol[10];
                sprintf(rlayer, "%6.4f", APP<Dtype>::pruned_ratio[L]);
                sprintf(rrow,   "%6.4f", APP<Dtype>::pruned_ratio_row[L]);
                sprintf(rcol,   "%6.4f", APP<Dtype>::pruned_ratio_col[L]);
                std::cout << layer_name << " prune finished!"
                          << "  step: " << APP<Dtype>::step_
                          << "  net speedup: " << APP<Dtype>::speedup
                          << "  net compRatio: " << APP<Dtype>::compRatio
                          << "  pruned_ratio: " << rlayer
                          << "  pruned_ratio_row: " << rrow
                          << "  pruned_ratio_col: " << rcol
                          << "  current prune_ratio: " << APP<Dtype>::current_prune_ratio[L] << std::endl;
                
                APP<Dtype>::IF_current_target_achieved = true;
                for (int i = 0; i < APP<Dtype>::conv_layer_cnt + APP<Dtype>::fc_layer_cnt; ++i) {
                    if (APP<Dtype>::iter_prune_finished[i] == INT_MAX) {
                        APP<Dtype>::IF_current_target_achieved = false;
                        break;
                    }
                }
                if (APP<Dtype>::IF_current_target_achieved) {
                    APP<Dtype>::stage_iter_prune_finished = APP<Dtype>::step_ - 1; 
                }
            }
        }
    }
}

// Update NumPruned Row
template <typename Dtype>
void Layer<Dtype>::UpdateNumPrunedRow() {
    const int L = APP<Dtype>::layer_index[this->layer_param_.name()];
    const int num_col = this->blobs_[0]->count(1);
    cout << "        " << this->layer_param_.name() << " in UpdateNumPrunedRow" << endl;
    vector<int>::iterator it;
    for (it = APP<Dtype>::rows_to_prune[L].begin(); it != APP<Dtype>::rows_to_prune[L].end(); ++it) {
        caffe_gpu_set(num_col, (Dtype)0, this->blobs_[0]->mutable_gpu_data() + *it * num_col);
        caffe_gpu_set(num_col, (Dtype)0, this->masks_[0]->mutable_gpu_data() + *it * num_col);
        APP<Dtype>::IF_row_pruned[L][*it] = true;
        cout << " " << this->layer_param_.name() << " prune a row successfully: " << (*it) << endl;
    }
    APP<Dtype>::num_pruned_row[L] += APP<Dtype>::rows_to_prune[L].size();
    APP<Dtype>::rows_to_prune[L].clear();
}

template <typename Dtype>
void Layer<Dtype>::UpdateNumPrunedCol() {
    const int L = APP<Dtype>::layer_index[this->layer_param_.name()];
    const int count   = this->blobs_[0]->count();
    const int num_row = this->blobs_[0]->shape()[0];
    const int num_col = count / num_row;
    const int num_chl = this->blobs_[0]->shape()[1];
    const int num_row_per_g = num_row / APP<Dtype>::group[L];
    const int filter_spatial_size = this->blobs_[0]->count(2);

    cout << "      " << this->layer_param_.name() << " in UpdateNumPrunedCol" << endl;
    vector<int>::iterator it;
    for (it = APP<Dtype>::pruned_rows[L-1].begin(); it != APP<Dtype>::pruned_rows[L-1].end(); ++it) {
        const int chl = *it % num_chl;
        const int g   = *it / num_chl;
        for (int i = g * num_row_per_g; i < (g + 1) * num_row_per_g; ++i) {
            for (int j = chl * filter_spatial_size; j < (chl + 1) * filter_spatial_size; ++j) {
                this->masks_[0]->mutable_cpu_data()[i * num_col + j] = 0;
                APP<Dtype>::IF_col_pruned[L][j][g] = true;
            }
        }
        APP<Dtype>::num_pruned_col[L] += filter_spatial_size * 1.0 / APP<Dtype>::group[L];
        cout << " " << this->layer_param_.name() << " prune a channel successfully: " << chl << endl;
    }
    APP<Dtype>::pruned_rows[L-1].clear();
}

template <typename Dtype>
void Layer<Dtype>::UpdatePrunedRatio() {
    const int L = APP<Dtype>::layer_index[this->layer_param_.name()];
    const int count   = this->blobs_[0]->count();
    const int num_row = this->blobs_[0]->shape()[0];
    const int num_col = count / num_row;
    // const int group = APP<Dtype>::group[L];
    // const Dtype* weight = this->blobs_[0]->cpu_data();

    APP<Dtype>::pruned_ratio_col[L] = APP<Dtype>::num_pruned_col[L] / num_col;
    APP<Dtype>::pruned_ratio_row[L] = APP<Dtype>::num_pruned_row[L] * 1.0 / num_row;
    APP<Dtype>::pruned_ratio_for_comparison[L] = APP<Dtype>::pruned_ratio_col[L];

    APP<Dtype>::pruned_ratio[L] = APP<Dtype>::pruned_ratio_col[L] + APP<Dtype>::pruned_ratio_row[L]
                                - APP<Dtype>::pruned_ratio_col[L] * APP<Dtype>::pruned_ratio_row[L];
    if (APP<Dtype>::prune_unit == "Row") {
        APP<Dtype>::pruned_ratio_for_comparison[L] = APP<Dtype>::pruned_ratio_row[L];
    }
}

template <typename Dtype>
void Layer<Dtype>::Print(char mode) {
    assert(mode == 'f' || mode == 'b');    /// forward, backward
    const string layer_name = this->layer_param_.name();
    const int num_col = this->blobs_[0]->count() / this->blobs_[0]->shape()[0];
    const int num_row = this->blobs_[0]->shape()[0];
    const Dtype* w = this->blobs_[0]->cpu_data();
    const Dtype* d = this->blobs_[0]->cpu_diff();
    const Dtype* m = this->masks_[0]->cpu_data();
    // print Index, blob, Mask
    cout.width(5);
    cout << "Index" << "   ";
    const string blob = (mode == 'f') ? "WeightBeforeMasked" : "DiffBeforeMasked";
    cout.width(blob.size());
    cout << blob << "   ";
    cout.width(4);
    cout << "Mask" << "   ";
    // print additional info
    string info = "";
    if (APP<Dtype>::prune_coremthd.substr(0, 2) == "PP") {
        info = "HistoryProb";
    }
    else if (APP<Dtype>::prune_coremthd.substr(0, 3) == "Reg") {
        info = "HistoryReg";
    }
    else {
        info = "WeightBeforeMasked";
    }
    Dtype* info_data = NULL;
    if (APP<Dtype>::prune_method.substr(0, 2) == "PP" || APP<Dtype>::prune_method.substr(0, 3) == "Reg") {
        info_data = this->history_punish_[0]->mutable_cpu_data();
    }
    else {
        info_data = this->blobs_[0]->mutable_cpu_data();
    }
    cout.width(info.size());
    cout << info << " - " << this->layer_param_.name() << endl;

    if (APP<Dtype>::prune_unit == "Row") {
        const int show_num = APP<Dtype>::show_num_weight > num_row ? num_row : APP<Dtype>::show_num_weight;
        for (int i = 0; i < show_num; ++i) {
            // print Index
            cout.width(3);
            cout << "r";
            cout.width(2);
            cout << i+1 << "   ";
            // print blob
            Dtype sum_w = 0, sum_d = 0;
            for (int j = 0; j < num_col; ++j) {
                sum_w += fabs(w[i * num_col + j]);
                sum_d += fabs(d[i * num_col + j]);
            }
            sum_w /= num_col;    /// average abs weight
            sum_d /= num_col;    /// average abs diff
            const Dtype s = mode == 'f' ? sum_w : sum_d;
            cout.width(blob.size());
            cout << s << "   ";
            // print Mask
            cout.width(4);
            cout << m[i * num_col] << "   ";
            // print info
            cout.width(info.size());
            cout << info_data[i * num_col] << endl;
        }
    }
    else if (APP<Dtype>::prune_unit == "Col") {
        const int show_num = APP<Dtype>::show_num_weight > num_col ? num_col : APP<Dtype>::show_num_weight;
        for (int j = 0; j < show_num; ++j) {
            // print Index
            cout.width(3);
            cout << "c";
            cout.width(2);
            cout << j+1 << "   ";
            // print blob
            Dtype sum_w = 0, sum_d = 0;
            for (int i = 0; i < num_row; ++i) {
                sum_w += fabs(w[i * num_col + j]);
                sum_d += fabs(d[i * num_col + j]);
            }
            sum_w /= num_row;     /// average abs weight
            sum_d /= num_row;     /// average abs diff
            const Dtype s = mode == 'f' ? sum_w : sum_d;
            cout.width(blob.size());
            cout << s << "   ";
            // print Mask
            cout.width(4);
            cout << m[j] << "   ";
            // print info
            cout.width(info.size());
            cout << info_data[j] << endl;
        }
    }
}

/// @luoyang
template <typename Dtype>
void Layer<Dtype>::RestoreMasks() {
    const int count   = this->blobs_[0]->count();
    const int num_row = this->blobs_[0]->shape()[0];
    const int num_col = count / num_row;
    const Dtype *weight = this->blobs_[0]->cpu_data();
    const string layer_name = this->layer_param_.name();
    const int L = APP<Dtype>::layer_index[layer_name];
    const int group = APP<Dtype>::group[L];
    const int num_row_per_g = num_row / group;
    const string mthd = APP<Dtype>::prune_method;
    Dtype num_pruned_col = 0;
    int   num_pruned_row = 0;

    // Clear existing pruning state
    caffe_gpu_set(this->masks_[0]->count(), Dtype(1), this->masks_[0]->mutable_gpu_data());
    APP<Dtype>::num_pruned_weight[L] = 0;
    APP<Dtype>::num_pruned_col[L]    = 0;
    APP<Dtype>::num_pruned_row[L]    = 0;
    vector<bool> vec_tmp(APP<Dtype>::group[L], false);
    APP<Dtype>::IF_col_pruned[L] = vector<vector<bool> >(num_col, vec_tmp);
    APP<Dtype>::IF_row_pruned[L] = vector<bool>(num_row, false);

    if (APP<Dtype>::prune_unit == "Weight") {
        for (int i = 0; i < count; ++i) {
            if (!weight[i]) {
                this->masks_[0]->mutable_cpu_data()[i] = 0;
                ++ APP<Dtype>::num_pruned_weight[L];
            }
        }
        //LOG(INFO) << layer_name << "  Masks restored, with unstructured masks";
    } 
    else {
        for (int j = 0; j < num_col; ++j) { // Column
            for (int g = 0; g < group; ++g) {
                Dtype sum = 0;
                for (int i = g * num_row_per_g; i < (g+1) * num_row_per_g; ++i) {
                    sum += fabs(weight[i * num_col + j]);
                }
                if (sum == 0) {
                    /// note that num_pruned_row is always integer while num_pruned_col can be non-integer because of group
                    num_pruned_col += 1.0 / group; 
                    APP<Dtype>::IF_col_pruned[L][j][g] = true;
                    for (int i = g * num_row_per_g; i < (g+1) * num_row_per_g; ++i) {
                        this->masks_[0]->mutable_cpu_data()[i * num_col + j] = 0;
                    } 
                }
            }
        }
        for (int i = 0; i < num_row; ++i) { // Row
            Dtype sum = 0;
            for (int j = 0; j < num_col; ++j) {
                sum += fabs(weight[i * num_col + j]);
            }
            if (sum == 0) {
                ++ num_pruned_row;
                APP<Dtype>::IF_row_pruned[L][i] = true;
                for (int j = 0; j < num_col; ++j) {
                    this->masks_[0]->mutable_cpu_data()[i * num_col + j] = 0;
                }
            }
        }
        APP<Dtype>::num_pruned_col[L] = num_pruned_col;
        APP<Dtype>::num_pruned_row[L] = num_pruned_row;
    }
    this->UpdatePrunedRatio();
    this->IF_layer_prune_finished();

    LOG(INFO) << "  Masks restored,"
              << "  num_pruned_col = " << APP<Dtype>::num_pruned_col[L] << "(" << APP<Dtype>::num_pruned_col[L] * 1.0 / num_col << ")"
              << "  num_pruned_row = " << APP<Dtype>::num_pruned_row[L] << "(" << APP<Dtype>::num_pruned_row[L] * 1.0 / num_row << ")"
              << "  pruned_ratio   = " << APP<Dtype>::pruned_ratio[L]
              << "  prune_ratio    = " << APP<Dtype>::prune_ratio[L];
              //<< "\n  **** Please check prune number here, compare it with wh's caffe logs ****";
}

template <typename Dtype>
void Layer<Dtype>::PruneSetUp(const PruneParameter& prune_param) {
    const int count = this->blobs_[0]->count();
    const int num_row = this->blobs_[0]->shape()[0];
    const int num_col = count / num_row;
    APP<Dtype>::prune_ratio.push_back(prune_param.prune_ratio());
    APP<Dtype>::prune_ratio_step.push_back(prune_param.prune_ratio_step());
    const Dtype current_pr_tmp = prune_param.prune_ratio();
    APP<Dtype>::current_prune_ratio.push_back(current_pr_tmp);
    APP<Dtype>::pruned_ratio.push_back(0);     // used in TEST
    if (this->phase_ == TEST) { return; }

    // Get layer index
    const string layer_name = this->layer_param_.name();
    if (APP<Dtype>::layer_index.count(layer_name) == 0) {
        APP<Dtype>::layer_index[layer_name] = APP<Dtype>::conv_layer_cnt + APP<Dtype>::fc_layer_cnt;
        if (!strcmp(this->type(), "Convolution")) {
            ++ APP<Dtype>::conv_layer_cnt;
        }
        else if (!strcmp(this->type(), "InnerProduct")) {
            ++ APP<Dtype>::fc_layer_cnt;
        }
        else {
            LOG(FATAL) << "Seems wrong, PruneSetUp can ONLY be put in the layers with learnable parameters (Conv and FC), please check.";
        }
        LOG(INFO) << "New learnable layer registered: " << layer_name
                  << ". Its layer index: " << APP<Dtype>::layer_index[layer_name] << endl;
    }
    const int L = APP<Dtype>::layer_index[layer_name];

    // Note: the varibales below can ONLY be used in training.
    // Set up prune parameters of layer
    APP<Dtype>::IF_update_row_col_layer.push_back(prune_param.if_update_row_col());
    APP<Dtype>::rows_to_prune.push_back(vector<int>());
    APP<Dtype>::pruned_rows.push_back(vector<int>());
    APP<Dtype>::pruned_ratio_col.push_back(0);
    APP<Dtype>::pruned_ratio_row.push_back(0);
    APP<Dtype>::pruned_ratio_for_comparison.push_back(0);
    APP<Dtype>::last_feasible_prune_ratio.push_back(0);
    APP<Dtype>::last_infeasible_prune_ratio.push_back(0);
    APP<Dtype>::GFLOPs.push_back(this->blobs_[0]->count());    // further calculated in 'net.cpp', after layer SetUp
    APP<Dtype>::num_param.push_back(count);
    // Pruning state
    APP<Dtype>::num_pruned_col.push_back(0);
    APP<Dtype>::num_pruned_row.push_back(0);
    APP<Dtype>::num_pruned_weight.push_back(0);
    APP<Dtype>::IF_row_pruned.push_back(vector<bool>(num_row, false));
    vector<bool> vec_tmp(APP<Dtype>::group[L], false);
    APP<Dtype>::IF_col_pruned.push_back(vector<vector<bool> >(num_col, vec_tmp));
    if (!strcmp(this->type(), "Convolution")) {
        if (APP<Dtype>::prune_unit == "Col") {
            APP<Dtype>::lambda.push_back(vector<Dtype>(num_col, 0));
        }
        else if (APP<Dtype>::prune_unit == "Row") {
            APP<Dtype>::lambda.push_back(vector<Dtype>(num_row, 0));
        }
    }
    // Info shared among layers
    APP<Dtype>::filter_spatial_size.push_back(this->blobs_[0]->shape()[2] * this->blobs_[0]->shape()[3]); // check 3D CNN
    APP<Dtype>::iter_prune_finished.push_back(INT_MAX);
    LOG(INFO) << "Pruning setup done: " << layer_name;
}

/// @luoyang
template <typename Dtype>
void Layer<Dtype>::PruneForward() {
    const int count = this->blobs_[0]->count();
    const int num_row = this->blobs_[0]->shape()[0];
    const int num_col = count / num_row;
    const string layer_name = this->layer_param_.name();
    const string mthd = APP<Dtype>::prune_method;
    const int L = APP<Dtype>::layer_index[layer_name];
    this->IF_restore = false;

    /// IF prune
    const bool IF_want_prune = mthd != "None" && APP<Dtype>::prune_ratio[L] > 0;
    const bool IF_been_pruned = APP<Dtype>::pruned_ratio[L] > 0;   // for a pruned layer, continue to prune
    const bool IF_enough_iter = APP<Dtype>::step_ >= APP<Dtype>::prune_begin_iter+1;  // for a raw layer, if iter is enough, prune
    const bool IF_prune = IF_want_prune && (IF_been_pruned || IF_enough_iter);

    if (this->phase_ == TRAIN && APP<Dtype>::inner_iter == 0) {
        // For a layer which doesn't want to prune, it still should UpdateNumPrunedCol/Row because of neighbour layer
        if (mthd != "None" && (IF_been_pruned || IF_enough_iter)) {
            if (APP<Dtype>::IF_update_row_col && APP<Dtype>::IF_update_row_col_layer[L]) {
                // Note that, UpdateNumPruneRow/Col before pruning
                // The last conv and last fc layer need not updating num of pruned row
                // In fact, the last conv should be updated row and the first fc should be update col
                if (APP<Dtype>::prune_unit == "Col" && L != APP<Dtype>::conv_layer_cnt - 1
                                                    && L != APP<Dtype>::conv_layer_cnt + APP<Dtype>::fc_layer_cnt - 1
                                                    && APP<Dtype>::rows_to_prune[L].size()) {
                    this->UpdateNumPrunedRow();
                }
                else if (APP<Dtype>::prune_unit == "Row" && L != 0
                                                         && L != APP<Dtype>::conv_layer_cnt  //The first convlayer not update column
                                                         && APP<Dtype>::pruned_rows[L-1].size()) {
                    this->UpdateNumPrunedCol();
                }
            }
        }
        this->UpdatePrunedRatio();
        this->IF_layer_prune_finished();

        // Print and check, before update probs
        // put this outside, to print even when we do not prune
        if (APP<Dtype>::prune_method != "None" && APP<Dtype>::show_layer.size() >= L+1 
                                               && APP<Dtype>::show_layer[L] == '1'
                                               && APP<Dtype>::step_ % APP<Dtype>::show_interval == 0) {
            this->Print('f');
        }

        // Summary print
        if (mthd != "None" && L < APP<Dtype>::show_num_layer) {
            cout << layer_name << "  IF_prune: " << IF_prune;
            if (APP<Dtype>::prune_unit == "Col") {
                cout << "  pruned_ratio_col: " << APP<Dtype>::num_pruned_col[L] * 1.0 / num_col
                     << "(" << APP<Dtype>::num_pruned_col[L] << ")";
            }
            else if (APP<Dtype>::prune_unit == "Row") {
                cout << "  pruned_ratio_row: " << APP<Dtype>::num_pruned_row[L] * 1.0 / num_row
                     << "(" << APP<Dtype>::num_pruned_row[L] << ")";
            }
            cout << "  current_prune_ratio: " << APP<Dtype>::current_prune_ratio[L];
            cout << "  iter_prune_finished: " << APP<Dtype>::iter_prune_finished[L];
            cout << "  (" << APP<Dtype>::prune_state;
            cout << "-" << APP<Dtype>::learning_rate;
            cout << "-" << APP<Dtype>::iter_size;
            cout << "-" << APP<Dtype>::target_reg;
            cout << ")" << endl;
        }

        // Apply masks
        if (mthd != "None") {
            caffe_gpu_mul(this->blobs_[0]->count(),
                          this->blobs_[0]->gpu_data(),
                          this->masks_[0]->gpu_data(),
                          this->blobs_[0]->mutable_gpu_data());
        }
    }
  
  //string self_prune_uint = this->layer_param_.prune_param().prune_unit();
  
  //if (self_prune_uint == "None") {
    //const string layer_name = this->layer_param_.name();
    //LOG(INFO) << layer_name << "Skipping restore of layer " << layer_name;
  //  return;
  //}
  
  //if (self_prune_uint != "Weight" && self_prune_uint != "Structure") {
  //  LOG(FATAL) << "Not support prune unit, check your protobuf";
  //}
  
  //const int count = this->blobs_[0]->count();
  //const int num_row = this->blobs_[0]->shape()[0];
  //const int num_col = count / num_row;
  //const string layer_name = this->layer_param_.name();
}

/// @luoyang
template <typename Dtype>
void Layer<Dtype>::PruneBackward() {
    const int L = APP<Dtype>::layer_index[this->layer_param_.name()];
    // Print and check
    if (APP<Dtype>::prune_method != "None" && APP<Dtype>::show_layer.size() >= L+1 && APP<Dtype>::show_layer[L] == '1'
                                           && APP<Dtype>::step_ % APP<Dtype>::show_interval == 0 && APP<Dtype>::inner_iter == 0) {
        this->Print('b');
    }
    // Apply masks to grads
    if (APP<Dtype>::pruned_ratio[L] > 0) {
        caffe_gpu_mul(this->blobs_[0]->count(),
                      this->blobs_[0]->gpu_diff(),
                      this->masks_[0]->gpu_data(),
                      this->blobs_[0]->mutable_gpu_diff());
    }
  
    //string self_prune_uint = this->layer_param_.prune_param().prune_unit();
  
    //if (self_prune_uint == "None") {
        //const string layer_name = this->layer_param_.name();
        //LOG(INFO) << layer_name << "Skipping restore of layer " << layer_name;
    //    return;
    //}
     
    //if (self_prune_uint != "Weight" && self_prune_uint != "Structure") {
    //    LOG(FATAL) << "Not support prune unit, check your protobuf";
    //}
}

/// @luoyang
//template <typename Dtype>
//string Layer<Dtype>::ReturnPruneUnit() {
//  string self_prune_uint = this->layer_param_.prune_param().prune_unit();
//  return self_prune_uint;
//}

INSTANTIATE_CLASS(Layer);

}  // namespace caffe
