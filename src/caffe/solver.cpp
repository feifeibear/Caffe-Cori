/*
All modification made by Intel Corporation: © 2016 Intel Corporation

All contributions by the University of California:
Copyright (c) 2014, 2015, The Regents of the University of California (Regents)
All rights reserved.

All other contributions:
Copyright (c) 2014, 2015, the respective contributors
All rights reserved.
For the list of contributors go to https://github.com/BVLC/caffe/blob/master/CONTRIBUTORS.md


Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <algorithm>
#include <map>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <fstream>

#include <numeric>

#include "boost/bind.hpp"
#include "caffe/solver.hpp"
#include "caffe/util/bbox_util.hpp"
#include "caffe/util/format.hpp"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/performance.hpp"
#include "caffe/util/upgrade_proto.hpp"

#ifdef USE_MLSL
#include <mlsl.h>
#include <mpi.h>
#endif /* USE_MLSL */
#ifdef USE_SELF_MPI
#include "caffe/util/mpi.hpp"
#endif


namespace caffe {
template<typename Dtype>
void Solver<Dtype>::copy_params_from_net(Dtype* params)
{
    std::vector<shared_ptr<Blob<Dtype> > > net_params = net_->params();
    int offset = 0;
    for (int i = 0; i < net_params.size(); ++i)
    {
        Blob<Dtype>* net_param = net_params[i].get();
        memcpy(params+offset, net_param->cpu_data(), sizeof(Dtype)*net_param->count());
        offset += net_param->count();
    }
}

template<typename Dtype>
void Solver<Dtype>::copy_diffs_from_net(Dtype* diffs)
{
    std::vector<shared_ptr<Blob<Dtype> > > net_params = net_->params();
    int offset = 0;
    for (int i = 0; i < net_params.size(); ++i)
    {
        Blob<Dtype>* net_param = net_params[i].get();
        memcpy(diffs+offset, net_param->cpu_diff(), sizeof(Dtype)*net_param->count());
        offset += net_param->count();
    }
}

template<typename Dtype>
void Solver<Dtype>::copy_params_to_net(Dtype* params)
{
    std::vector<shared_ptr<Blob<Dtype> > > net_params = net_->params();
    int offset = 0;
    for (int i = 0; i < net_params.size(); ++i)
    {
        Blob<Dtype>* net_param = net_params[i].get();
        net_param->set_cpu_data(params + offset);
        offset += net_param->count();
    }
}

template<typename Dtype>
void Solver<Dtype>::copy_diffs_to_net(Dtype* diffs)
{
    std::vector<shared_ptr<Blob<Dtype> > > net_params = net_->params();
    int offset = 0;
    for (int i = 0; i < net_params.size(); ++i)
    {
        Blob<Dtype>* net_param = net_params[i].get();
        //fjr
        //net_param->set_cpu_diff(diffs + offset);
        memcpy(diffs+offset, net_param->mutable_cpu_diff(), sizeof(Dtype)*net_param->count());
        offset += net_param->count();
    }
}

template<typename Dtype>
void Solver<Dtype>::find_net_size(int & param_size)
{
      std::vector<shared_ptr<Blob<Dtype> > > net_params = net_->params();
      param_size = 0;
      for (int i = 0; i < net_params.size(); ++i)
      {
          const Blob<Dtype>* net_param = net_params[i].get();
          param_size += net_param->count();
      }
}

template<typename Dtype>
void Solver<Dtype>::SetActionFunction(ActionCallback func) {
  action_request_function_ = func;
}

template<typename Dtype>
SolverAction::Enum Solver<Dtype>::GetRequestedAction() {
  if (action_request_function_) {
    // If the external request function has been set, call it.
    return action_request_function_();
  }
  return SolverAction::NONE;
}

template <typename Dtype>
Solver<Dtype>::Solver(const SolverParameter& param, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false),
      forward_backward_(boost::bind(&Solver<Dtype>::ForwardBackward, this)) {
  Init(param);
  Caffe::set_iter_size(param_.iter_size());
  LOG(INFO) << "debug inside Solver";
}

template <typename Dtype>
Solver<Dtype>::Solver(const string& param_file, const Solver* root_solver)
    : net_(), callbacks_(), root_solver_(root_solver),
      requested_early_exit_(false),
      forward_backward_(boost::bind(&Solver<Dtype>::ForwardBackward, this)) {
  SolverParameter param;
  std::cout << "Solver Construction" <<std::endl;
  ReadSolverParamsFromTextFileOrDie(param_file, &param);
  Init(param);
  Caffe::set_iter_size(param_.iter_size());
}

template <typename Dtype>
void Solver<Dtype>::Init(const SolverParameter& param) {
  CHECK(Caffe::root_solver() || root_solver_)
      << "root_solver_ needs to be set for all non-root solvers";
  param_ = param;

#ifdef USE_MLSL
  ReplaceMultinodeSolverParams(&param_);
#endif

  LOG_IF(INFO, Caffe::root_solver()) << "Initializing solver from parameters: "
    << std::endl << param_.DebugString();

  CHECK_GE(param_.average_loss(), 1) << "average_loss should be non-negative.";
#ifndef USE_MLSL
  CheckSnapshotWritePermissions();
#endif
  if (Caffe::root_solver() && param_.random_seed() >= 0) {
    Caffe::set_random_seed(param_.random_seed());
  }
  // Scaffolding code
  InitTrainNet();
  if (Caffe::root_solver()) {
    InitTestNets();
    LOG(INFO) << "Solver scaffolding done.";
  }
  iter_ = 0;
  current_step_ = 0;

#ifdef CAFFE_PER_LAYER_TIMINGS
  InitTimers();
#endif
}

template <typename Dtype>
void Solver<Dtype>::InitTrainNet() {
  const int num_train_nets = param_.has_net() + param_.has_net_param() +
      param_.has_train_net() + param_.has_train_net_param();
  const string& field_names = "net, net_param, train_net, train_net_param";
  CHECK_GE(num_train_nets, 1) << "SolverParameter must specify a train net "
      << "using one of these fields: " << field_names;
  CHECK_LE(num_train_nets, 1) << "SolverParameter must not contain more than "
      << "one of these fields specifying a train_net: " << field_names;
  NetParameter net_param;
  if (param_.has_train_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in train_net_param.";
    net_param.CopyFrom(param_.train_net_param());
  } else if (param_.has_train_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from train_net file: " << param_.train_net();
    ReadNetParamsFromTextFileOrDie(param_.train_net(), &net_param);
  }
  if (param_.has_net_param()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net specified in net_param.";
    net_param.CopyFrom(param_.net_param());
  }
  if (param_.has_net()) {
    LOG_IF(INFO, Caffe::root_solver())
        << "Creating training net from net file: " << param_.net();
    ReadNetParamsFromTextFileOrDie(param_.net(), &net_param);
  }
  if (param_.engine() != "")
    net_param.set_engine(param_.engine());
  // Set the correct NetState.  We start with the solver defaults (lowest
  // precedence); then, merge in any NetState specified by the net_param itself;
  // finally, merge in any NetState specified by the train_state (highest
  // precedence).
  NetState net_state;
  net_state.set_phase(TRAIN);
  net_state.MergeFrom(net_param.state());
  net_state.MergeFrom(param_.train_state());
  net_param.mutable_state()->CopyFrom(net_state);
  if (Caffe::root_solver()) {
    net_.reset(new Net<Dtype>(net_param));
  } else {
    net_.reset(new Net<Dtype>(net_param, root_solver_->net_.get()));
  }
}

template <typename Dtype>
void Solver<Dtype>::InitTestNets() {
  CHECK(Caffe::root_solver());
  const bool has_net_param = param_.has_net_param();
  const bool has_net_file = param_.has_net();
  const int num_generic_nets = has_net_param + has_net_file;
  CHECK_LE(num_generic_nets, 1)
      << "Both net_param and net_file may not be specified.";
  const int num_test_net_params = param_.test_net_param_size();
  const int num_test_net_files = param_.test_net_size();
  const int num_test_nets = num_test_net_params + num_test_net_files;
  if (num_generic_nets) {
      CHECK_GE(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  } else {
      CHECK_EQ(param_.test_iter_size(), num_test_nets)
          << "test_iter must be specified for each test network.";
  }
  // If we have a generic net (specified by net or net_param, rather than
  // test_net or test_net_param), we may have an unlimited number of actual
  // test networks -- the actual number is given by the number of remaining
  // test_iters after any test nets specified by test_net_param and/or test_net
  // are evaluated.
  const int num_generic_net_instances = param_.test_iter_size() - num_test_nets;
  const int num_test_net_instances = num_test_nets + num_generic_net_instances;
  if (param_.test_state_size()) {
    CHECK_EQ(param_.test_state_size(), num_test_net_instances)
        << "test_state must be unspecified or specified once per test net.";
  }
  if (num_test_net_instances) {
    CHECK_GT(param_.test_interval(), 0);
  }
  int test_net_id = 0;
  vector<string> sources(num_test_net_instances);
  vector<NetParameter> net_params(num_test_net_instances);
  for (int i = 0; i < num_test_net_params; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net_param";
      net_params[test_net_id].CopyFrom(param_.test_net_param(i));
  }
  for (int i = 0; i < num_test_net_files; ++i, ++test_net_id) {
      sources[test_net_id] = "test_net file: " + param_.test_net(i);
      ReadNetParamsFromTextFileOrDie(param_.test_net(i),
          &net_params[test_net_id]);
  }
  const int remaining_test_nets = param_.test_iter_size() - test_net_id;
  if (has_net_param) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net_param";
      net_params[test_net_id].CopyFrom(param_.net_param());
    }
  }
  if (has_net_file) {
    for (int i = 0; i < remaining_test_nets; ++i, ++test_net_id) {
      sources[test_net_id] = "net file: " + param_.net();
      ReadNetParamsFromTextFileOrDie(param_.net(), &net_params[test_net_id]);
    }
  }
  test_nets_.resize(num_test_net_instances);
  for (int i = 0; i < num_test_net_instances; ++i) {
    // Set the correct NetState.  We start with the solver defaults (lowest
    // precedence); then, merge in any NetState specified by the net_param
    // itself; finally, merge in any NetState specified by the test_state
    // (highest precedence).
    NetState net_state;
    net_state.set_phase(TEST);
    net_state.MergeFrom(net_params[i].state());
    if (param_.test_state_size()) {
      net_state.MergeFrom(param_.test_state(i));
    }
    net_params[i].mutable_state()->CopyFrom(net_state);

    if (param_.engine() != "")
      net_params[i].set_engine(param_.engine());

    LOG(INFO)
        << "Creating test net (#" << i << ") specified by " << sources[i];
    if (Caffe::root_solver()) {
      test_nets_[i].reset(new Net<Dtype>(net_params[i]));
    } else {
      test_nets_[i].reset(new Net<Dtype>(net_params[i],
          root_solver_->test_nets_[i].get()));
    }
    test_nets_[i]->set_debug_info(param_.debug_info());
  }
}

template <typename Dtype>
Dtype Solver<Dtype>::ForwardBackward() {
  // zero-init the params
  net_->ClearParamDiffs();

  Dtype loss = Dtype();
  vector<Blob<Dtype>*> bottom_vec;

  // accumulate the loss and gradient
  for (int i = 0; i < param_.iter_size(); ++i) {
    loss += net_->ForwardBackward();
  }
  return loss / param_.iter_size();
}

template <typename Dtype>
void Solver<Dtype>::Step(int iters) {
  LOG(INFO) << "inside Step";
#ifdef USE_SELF_MPI
  LOG(INFO) << "inside Step USE_SELF_MPI";
	int world_size;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
	int world_rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
	char processor_name[MPI_MAX_PROCESSOR_NAME];
  int name_len;
  MPI_Get_processor_name(processor_name, &name_len);

  printf("Hello world from processor %s, rank %d" " out of %d processors\n", processor_name, world_rank, world_size);

	shared_ptr<Net<Dtype> > myNet;
	myNet = net();
	int param_size;
	find_net_size(param_size);
	printf("****** rank %d: number of layers is %d, paramter size is %d ******\n", world_rank, (int)(myNet->params().size()), param_size);

	Dtype * local_param = (Dtype *)malloc(param_size*sizeof(Dtype));
	Dtype * local_diff  = (Dtype *)malloc(param_size*sizeof(Dtype));
	Dtype * global_diff = (Dtype *)malloc(param_size*sizeof(Dtype));

	copy_params_from_net(local_param);
	caffe_mpi_bcast<Dtype>(local_param, param_size, 0, MPI_COMM_WORLD);
	copy_params_to_net(local_param);

#endif
  const int start_iter = iter_;
  const int stop_iter = iter_ + iters;
  int average_loss = this->param_.average_loss();
  losses_.clear();
  smoothed_loss_ = 0;

  while (iter_ < stop_iter) {
    if (param_.test_interval() && iter_ % param_.test_interval() == 0
        && (iter_ > 0 || param_.test_initialization())
        && Caffe::root_solver()) {
      TestAll();
      if (requested_early_exit_) {
        // Break out of the while loop because stop was requested while testing.
        break;
      }
    }

    for (int i = 0; i < callbacks_.size(); ++i) {
      callbacks_[i]->on_start();
    }
    const bool display = param_.display() && iter_ % param_.display() == 0;
    net_->set_debug_info(display && param_.debug_info());

    Timer iter_timer;
    double iter_time = 0;
    iter_timer.Start();

    Dtype loss = forward_backward_();

    iter_time += iter_timer.MilliSeconds();

    // average the loss across iterations for smoothed reporting
    UpdateSmoothedLoss(loss, start_iter, average_loss);
    my_loss = smoothed_loss_;
    if (display) {
      LOG_IF(INFO, Caffe::root_solver()) << "Iteration " << iter_ << ", loss = " << smoothed_loss_;
      string lossname = "loss" + std::to_string(mpi_rank) + ".txt";
      std::ofstream floss;
      floss.open(lossname, std::ofstream::out | std::ofstream::app);
      floss << smoothed_loss_ << " ";
      floss.close();
      const vector<Blob<Dtype>*>& result = net_->output_blobs();
      int score_index = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        const string& output_name =
            net_->blob_names()[net_->output_blob_indices()[j]];
        const Dtype loss_weight =
            net_->blob_loss_weights()[net_->output_blob_indices()[j]];
        for (int k = 0; k < result[j]->count(); ++k) {
          ostringstream loss_msg_stream;
          if (loss_weight) {
            loss_msg_stream << " (* " << loss_weight
                            << " = " << loss_weight * result_vec[k] << " loss)";
          }
          LOG_IF(INFO, Caffe::root_solver()) << "    Train net output #"
              << score_index++ << ": " << output_name << " = "
              << result_vec[k] << loss_msg_stream.str();
        }
      }

#ifdef CAFFE_PER_LAYER_TIMINGS
      PrintTimers(false);
      ResetTimers();
//      MLSL::print_mlsl_time();
#endif
    }

    iter_timer.Start();

    for (int i = 0; i < callbacks_.size(); ++i) {
      callbacks_[i]->on_gradients_ready();
    }
    if (!param().disabled_update()) {
      PERFORMANCE_MEASUREMENT_BEGIN();
      ApplyUpdate();
      PERFORMANCE_MEASUREMENT_END_STATIC("weights_update");
    }

    iter_time += iter_timer.MilliSeconds();

#ifdef CAFFE_PER_LAYER_TIMINGS
    if (MLSL::GetNodeId() == 0)
        LOG(INFO) << "iter " << iter_ << ", forward_backward_update_time: "
                << iter_time << " ms";
#endif

    // Increment the internal iter_ counter -- its value should always indicate
    // the number of times the weights have been updated.
    ++iter_;

    SolverAction::Enum request = GetRequestedAction();

    // Save a snapshot if needed.
    if ((param_.snapshot()
         && iter_ % param_.snapshot() == 0
         && Caffe::root_solver()) ||
         (request == SolverAction::SNAPSHOT)) {
      Snapshot();
    }
    if (SolverAction::STOP == request) {
      requested_early_exit_ = true;
      // Break out of training loop.
      break;
    }
#ifdef USE_SELF_MPI
		copy_diffs_from_net(local_diff);
		caffe_mpi_allreduce<Dtype>(local_diff, global_diff, param_size, MPI_SUM, MPI_COMM_WORLD);
    LOG(INFO) << "USE_SELF_MPI allreduce ";
		#pragma omp parallel for
		#pragma simd
		for(int j=0;j<param_size;j++)
			local_param[j] -= global_diff[j];

		copy_params_to_net(local_param);
#elif
    LOG(INFO) << "NOT USE_SELF_MPI allreduce ";
#endif
    LOG(INFO) << "END USE_SELF_MPI allreduce ";
  }

#ifdef CAFFE_PER_LAYER_TIMINGS
  ResetTimers();
  PrintTimers(true);
#endif
}

#ifdef CAFFE_PER_LAYER_TIMINGS

template <typename Dtype>
void Solver<Dtype>::InitTimers() {
  int layer_count = net_->layers().size();

  this->forward_time_per_layer.resize(layer_count, 0.0);
  this->backward_time_per_layer.resize(layer_count, 0.0);
  this->update_time_per_layer.resize(layer_count, 0.0);

  this->forward_time_per_layer_total.resize(layer_count, 0.0);
  this->backward_time_per_layer_total.resize(layer_count, 0.0);
  this->update_time_per_layer_total.resize(layer_count, 0.0);
}

template <typename Dtype>
void Solver<Dtype>::ResetTimers() {
  std::transform(this->forward_time_per_layer_total.begin(),
                 this->forward_time_per_layer_total.end(),
                 this->forward_time_per_layer.begin(),
                 this->forward_time_per_layer_total.begin(),
                 std::plus<int>());

  std::transform(this->backward_time_per_layer_total.begin(),
                 this->backward_time_per_layer_total.end(),
                 this->backward_time_per_layer.begin(),
                 this->backward_time_per_layer_total.begin(),
                 std::plus<int>());

  std::transform(this->update_time_per_layer_total.begin(),
                 this->update_time_per_layer_total.end(),
                 this->update_time_per_layer.begin(),
                 this->update_time_per_layer_total.begin(),
                 std::plus<int>());

  std::fill(this->forward_time_per_layer.begin(),
          this->forward_time_per_layer.end(), 0);
  std::fill(this->backward_time_per_layer.begin(),
          this->backward_time_per_layer.end(), 0);
  std::fill(this->update_time_per_layer.begin(),
          this->update_time_per_layer.end(), 0);
}

template <typename Dtype>
void Solver<Dtype>::PrintTimers(bool printTotal) {
#ifdef USE_MLSL
    if (MLSL::GetNodeId())
       return;
#endif

    LOG(WARNING) << std::endl;
    LOG(WARNING) << "####################################################";

    std::vector<double>& forward_timers = printTotal ?
        forward_time_per_layer_total : forward_time_per_layer;
    std::vector<double>& backward_timers = printTotal ?
        backward_time_per_layer_total : backward_time_per_layer;
    std::vector<double>& update_timers = printTotal ?
        update_time_per_layer_total : update_time_per_layer;
    std::string prefix = printTotal ? "TOTAL " : "DELTA ";

    double forward_time = std::accumulate(forward_timers.begin(),
            forward_timers.end(), 0) / 1000;
    LOG(WARNING) << prefix << "FORWARD TIME: " << forward_time << " ms";
    for (int layer_idx = 0; layer_idx < net_->layers().size(); layer_idx++) {
        LOG(WARNING) << "LAYER-" << layer_idx << " "
                     << net_->layers()[layer_idx]->type()
                     << ": forward_time: " << forward_timers[layer_idx] / 1000
                     << " ms";
    }
    LOG(WARNING) << std::endl;

    double backward_time = std::accumulate(backward_timers.begin(),
            backward_timers.end(), 0) / 1000;
    LOG(WARNING) << prefix << "BACKWARD TIME: " << backward_time << " ms";
    for (int layer_idx = 0; layer_idx < net_->layers().size(); layer_idx++) {
        LOG(WARNING) << "LAYER-" << layer_idx << " "
                     << net_->layers()[layer_idx]->type()
                     << ": backward_time: " << backward_timers[layer_idx] / 1000
                     << " ms";
    }
    LOG(WARNING) << std::endl;

    double update_time = std::accumulate(update_timers.begin(),
            update_timers.end(), 0) / 1000;
    LOG(WARNING) << prefix << "UPDATE TIME: " << update_time << " ms";
    for (int layer_idx = 0; layer_idx < net_->layers().size(); layer_idx++) {
        LOG(WARNING) << "LAYER-" << layer_idx << " "
                     << net_->layers()[layer_idx]->type()
                     << ": update_time: " << update_timers[layer_idx] / 1000
                     << " ms";
    }
    LOG(WARNING) << std::endl;

    LOG(WARNING) << prefix << "TIME (F+B+U): " << (forward_time +
            backward_time + update_time) / 1000 << " sec";
    LOG(WARNING) << "####################################################";
    LOG(WARNING) << std::endl;
}

#endif /* CAFFE_PER_LAYER_TIMINGS */

template <typename Dtype>
void Solver<Dtype>::Solve(const char* resume_file) {
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Solving " << net_->name();
  LOG(INFO) << "Learning Rate Policy: " << param_.lr_policy();

  PERFORMANCE_INIT_MONITOR();

  // Initialize to false every time we start solving.
  requested_early_exit_ = false;

  if (resume_file) {
    LOG(INFO) << "Restoring previous solver status from " << resume_file;
    Restore(resume_file);
  }

  // For a network that is trained by the solver, no bottom or top vecs
  // should be given, and we will just provide dummy vecs.
  int start_iter = iter_;
  Step(param_.max_iter() - iter_);
  // If we haven't already, save a snapshot after optimization, unless
  // overridden by setting snapshot_after_train := false
  if (param_.snapshot_after_train()
      && (!param_.snapshot() || iter_ % param_.snapshot() != 0)) {
    Snapshot();
  }
  if (requested_early_exit_) {
    LOG(INFO) << "Optimization stopped early.";
    return;
  }
  // After the optimization is done, run an additional train and test pass to
  // display the train and test loss/outputs if appropriate (based on the
  // display and test_interval settings, respectively).  Unlike in the rest of
  // training, for the train net we only run a forward pass as we've already
  // updated the parameters "max_iter" times -- this final pass is only done to
  // display the loss, which is computed in the forward pass.
  if (param_.display() && iter_ % param_.display() == 0) {
    int average_loss = this->param_.average_loss();
    Dtype loss;
    net_->Forward(&loss);

    UpdateSmoothedLoss(loss, start_iter, average_loss);
    LOG(INFO) << "Iteration " << iter_ << ", loss = " << smoothed_loss_;
  }

#ifdef USE_MLSL
  // in multinode last test must be done after weights update
  if (param_.test_interval() && iter_ % param_.test_interval() == 0)
    TestAll();
#endif

  LOG(INFO) << "Optimization Done.";
}

template <typename Dtype>
void Solver<Dtype>::TestAll() {
  for (int test_net_id = 0;
       test_net_id < test_nets_.size() && !requested_early_exit_;
       ++test_net_id) {
    if (param_.eval_type() == "classification") {
      TestClassification(test_net_id);
    } else if (param_.eval_type() == "detection") {
      TestDetection(test_net_id);
    } else {
      LOG(FATAL) << "Unknown evaluation type: " << param_.eval_type();
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::TestClassification(const int test_net_id) {
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Iteration " << iter_
            << ", Testing net (#" << test_net_id << ")";
  CHECK_NOTNULL(test_nets_[test_net_id].get())->
      ShareTrainedLayersWith(net_.get());
  vector<Dtype> test_score;
  vector<int> test_score_output_id;
  const shared_ptr<Net<Dtype> >& test_net = test_nets_[test_net_id];
  Dtype loss = 0;
  for (int i = 0; i < param_.test_iter(test_net_id); ++i) {
    SolverAction::Enum request = GetRequestedAction();
    // Check to see if stoppage of testing/training has been requested.
    while (request != SolverAction::NONE) {
        if (SolverAction::SNAPSHOT == request) {
          Snapshot();
        } else if (SolverAction::STOP == request) {
          requested_early_exit_ = true;
        }
        request = GetRequestedAction();
    }
    if (requested_early_exit_) {
      // break out of test loop.
      break;
    }

    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result =
        test_net->Forward(&iter_loss);
    if (param_.test_compute_loss()) {
      loss += iter_loss;
    }
    if (i == 0) {
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score.push_back(result_vec[k]);
          test_score_output_id.push_back(j);
        }
      }
    } else {
      int idx = 0;
      for (int j = 0; j < result.size(); ++j) {
        const Dtype* result_vec = result[j]->cpu_data();
        for (int k = 0; k < result[j]->count(); ++k) {
          test_score[idx++] += result_vec[k];
        }
      }
    }
  }
  if (requested_early_exit_) {
    LOG(INFO)     << "Test interrupted.";
    return;
  }
  if (param_.test_compute_loss()) {
#ifdef USE_MLSL
    MPI_Allreduce(MPI_IN_PLACE, &loss, 1, sizeof(Dtype) == 4 ?
        MPI_FLOAT : MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    loss /= (param_.test_iter(test_net_id) * MLSL::GetNumNodes());
    if (MLSL::GetNodeId() == 0) LOG(INFO) << "Test loss: " << loss;
#else /* !USE_MLSL */
    loss /= param_.test_iter(test_net_id);
    LOG(INFO) << "Test loss: " << loss;
#endif /* USE_MLSL */
  }
#ifdef USE_MLSL
  MPI_Allreduce(MPI_IN_PLACE, test_score.data(), test_score.size(),
          sizeof(Dtype) == 4 ? MPI_FLOAT : MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
  if (MLSL::GetNodeId() == 0)
#endif /* USE_MLSL */
  for (int i = 0; i < test_score.size(); ++i) {
    const int output_blob_index =
        test_net->output_blob_indices()[test_score_output_id[i]];
    const string& output_name = test_net->blob_names()[output_blob_index];
    const Dtype loss_weight = test_net->blob_loss_weights()[output_blob_index];
    ostringstream loss_msg_stream;
#ifdef USE_MLSL
    const Dtype mean_score =
      test_score[i] / (param_.test_iter(test_net_id) * MLSL::GetNumNodes());
#else /* !USE_MLSL */
    const Dtype mean_score = test_score[i] / param_.test_iter(test_net_id);
#endif /* USE_MLSL */
    if (loss_weight) {
      loss_msg_stream << " (* " << loss_weight
                      << " = " << loss_weight * mean_score << " loss)";
    }
    LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
              << mean_score << loss_msg_stream.str();
    if(i==0)
    {
      string accuracyname = "accuracy" + std::to_string(mpi_rank) + ".txt";
      std::ofstream faccu;
      faccu.open(accuracyname, std::ofstream::out | std::ofstream::app);
      my_accuracy = mean_score;
      faccu << mean_score << " ";
      faccu.close();
    }
  }
}

template <typename Dtype>
void Solver<Dtype>::TestDetection(const int test_net_id) {
  CHECK(Caffe::root_solver());
  LOG(INFO) << "Iteration " << iter_
            << ", Testing net (#" << test_net_id << ")";
  CHECK_NOTNULL(test_nets_[test_net_id].get())->
      ShareTrainedLayersWith(net_.get());
  map<int, map<int, vector<pair<float, int> > > > all_true_pos;
  map<int, map<int, vector<pair<float, int> > > > all_false_pos;
  map<int, map<int, int> > all_num_pos;
  const shared_ptr<Net<Dtype> >& test_net = test_nets_[test_net_id];
  Dtype loss = 0;
  for (int i = 0; i < param_.test_iter(test_net_id); ++i) {
    SolverAction::Enum request = GetRequestedAction();
    // Check to see if stoppage of testing/training has been requested.
    while (request != SolverAction::NONE) {
        if (SolverAction::SNAPSHOT == request) {
          Snapshot();
        } else if (SolverAction::STOP == request) {
          requested_early_exit_ = true;
        }
        request = GetRequestedAction();
    }
    if (requested_early_exit_) {
      // break out of test loop.
      break;
    }

    Dtype iter_loss;
    const vector<Blob<Dtype>*>& result = test_net->Forward(&iter_loss);
    if (param_.test_compute_loss()) {
      loss += iter_loss;
    }
    for (int j = 0; j < result.size(); ++j) {
      CHECK_EQ(result[j]->width(), 5);
      const Dtype* result_vec = result[j]->cpu_data();
      int num_det = result[j]->height();
      for (int k = 0; k < num_det; ++k) {
        int item_id = static_cast<int>(result_vec[k * 5]);
        int label = static_cast<int>(result_vec[k * 5 + 1]);
        if (item_id == -1) {
          // Special row of storing number of positives for a label.
          if (all_num_pos[j].find(label) == all_num_pos[j].end()) {
            all_num_pos[j][label] = static_cast<int>(result_vec[k * 5 + 2]);
          } else {
            all_num_pos[j][label] += static_cast<int>(result_vec[k * 5 + 2]);
          }
        } else {
          // Normal row storing detection status.
          float score = result_vec[k * 5 + 2];
          int tp = static_cast<int>(result_vec[k * 5 + 3]);
          int fp = static_cast<int>(result_vec[k * 5 + 4]);
          if (tp == 0 && fp == 0) {
            // Ignore such case. It happens when a detection bbox is matched to
            // a difficult gt bbox and we don't evaluate on difficult gt bbox.
            continue;
          }
          all_true_pos[j][label].push_back(std::make_pair(score, tp));
          all_false_pos[j][label].push_back(std::make_pair(score, fp));
        }
      }
    }
  }
  if (requested_early_exit_) {
    LOG(INFO)     << "Test interrupted.";
    return;
  }
  if (param_.test_compute_loss()) {
    loss /= param_.test_iter(test_net_id);
    LOG(INFO) << "Test loss: " << loss;
  }
  for (int i = 0; i < all_true_pos.size(); ++i) {
    if (all_true_pos.find(i) == all_true_pos.end()) {
      LOG(FATAL) << "Missing output_blob true_pos: " << i;
    }
    const map<int, vector<pair<float, int> > >& true_pos =
        all_true_pos.find(i)->second;
    if (all_false_pos.find(i) == all_false_pos.end()) {
      LOG(FATAL) << "Missing output_blob false_pos: " << i;
    }
    const map<int, vector<pair<float, int> > >& false_pos =
        all_false_pos.find(i)->second;
    if (all_num_pos.find(i) == all_num_pos.end()) {
      LOG(FATAL) << "Missing output_blob num_pos: " << i;
    }
    const map<int, int>& num_pos = all_num_pos.find(i)->second;
    map<int, float> APs;
    float mAP = 0.;
    // Sort true_pos and false_pos with descend scores.
    for (map<int, int>::const_iterator it = num_pos.begin();
         it != num_pos.end(); ++it) {
      int label = it->first;
      int label_num_pos = it->second;
      if (true_pos.find(label) == true_pos.end()) {
        LOG(WARNING) << "Missing true_pos for label: " << label;
        continue;
      }
      const vector<pair<float, int> >& label_true_pos =
          true_pos.find(label)->second;
      if (false_pos.find(label) == false_pos.end()) {
        LOG(WARNING) << "Missing false_pos for label: " << label;
        continue;
      }
      const vector<pair<float, int> >& label_false_pos =
          false_pos.find(label)->second;
      vector<float> prec, rec;
      ComputeAP(label_true_pos, label_num_pos, label_false_pos,
                param_.ap_version(), &prec, &rec, &(APs[label]));
      mAP += APs[label];
    }
    mAP /= num_pos.size();
    const int output_blob_index = test_net->output_blob_indices()[i];
    const string& output_name = test_net->blob_names()[output_blob_index];
    LOG(INFO) << "    Test net output #" << i << ": " << output_name << " = "
              << mAP;
  }
}

template <typename Dtype>
void Solver<Dtype>::Snapshot() {
  CHECK(Caffe::root_solver());

#ifdef USE_MLSL
  if (MLSL::GetNodeId() != 0) return;
#endif /* USE_MLSL */

  string model_filename;
  switch (param_.snapshot_format()) {
  case caffe::SolverParameter_SnapshotFormat_BINARYPROTO:
    model_filename = SnapshotToBinaryProto();
    break;
  case caffe::SolverParameter_SnapshotFormat_HDF5:
    model_filename = SnapshotToHDF5();
    break;
  default:
    LOG(FATAL) << "Unsupported snapshot format.";
  }

  SnapshotSolverState(model_filename);
}

template <typename Dtype>
void Solver<Dtype>::CheckSnapshotWritePermissions() {
  if (Caffe::root_solver() && param_.snapshot()) {
    CHECK(param_.has_snapshot_prefix())
        << "In solver params, snapshot is specified but snapshot_prefix is not";
    string probe_filename = SnapshotFilename(".tempfile");
    std::ofstream probe_ofs(probe_filename.c_str());
    if (probe_ofs.good()) {
      probe_ofs.close();
      std::remove(probe_filename.c_str());
    } else {
      LOG(FATAL) << "Cannot write to snapshot prefix '"
          << param_.snapshot_prefix() << "'.  Make sure "
          << "that the directory exists and is writeable.";
    }
  }
}

template <typename Dtype>
string Solver<Dtype>::SnapshotFilename(const string extension) {
  return param_.snapshot_prefix() + "_iter_" + caffe::format_int(iter_)
    + extension;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToBinaryProto() {
  string model_filename = SnapshotFilename(".caffemodel");
  LOG(INFO) << "Snapshotting to binary proto file " << model_filename;
  NetParameter net_param;
  net_->ToProto(&net_param, param_.snapshot_diff());
  WriteProtoToBinaryFile(net_param, model_filename);
  return model_filename;
}

template <typename Dtype>
string Solver<Dtype>::SnapshotToHDF5() {
  string model_filename = SnapshotFilename(".caffemodel.h5");
  LOG(INFO) << "Snapshotting to HDF5 file " << model_filename;
  net_->ToHDF5(model_filename, param_.snapshot_diff());
  return model_filename;
}

template <typename Dtype>
void Solver<Dtype>::Restore(const char* state_file) {
  CHECK(Caffe::root_solver());
  string state_filename(state_file);
  if (state_filename.size() >= 3 &&
      state_filename.compare(state_filename.size() - 3, 3, ".h5") == 0) {
    RestoreSolverStateFromHDF5(state_filename);
  } else {
    RestoreSolverStateFromBinaryProto(state_filename);
  }
}

template <typename Dtype>
void Solver<Dtype>::UpdateSmoothedLoss(Dtype loss, int start_iter,
    int average_loss) {
  if (losses_.size() < average_loss) {
    losses_.push_back(loss);
    int size = losses_.size();
    smoothed_loss_ = (smoothed_loss_ * (size - 1) + loss) / size;
  } else {
    int idx = (iter_ - start_iter) % average_loss;
    smoothed_loss_ += (loss - losses_[idx]) / average_loss;
    losses_[idx] = loss;
  }
}

INSTANTIATE_CLASS(Solver);

}  // namespace caffe
