/*************************************************************************
 * Copyright (C) [2022] by Cambricon, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *************************************************************************/
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <unordered_set>
#include <memory>
#include <utility>
#include "executor.h"
#include "time.h"
#include "core/runtime/device.h"

#define GTEST_DEBUG_ENABLE 0

namespace mluoptest {
Executor::~Executor() {
  // these function can't throw exception.
  VLOG(4) << "Free all resource.";
  mluOutputFree();
  deviceFree();
  hostFree();
  baselineFree();
  destroyTensors();
  VLOG(4) << "Executor end.";
}

void Executor::init(const std::shared_ptr<ExecuteContext> ectx) {
  VLOG(4) << "Executor start.";
  exe_context_ = ectx;

  cpu_runtime_.init(exe_context_->cmp);
  mlu_runtime_.init(exe_context_->mmp);

  handle_ = exe_context_->handle;
  queue_ = exe_context_->queue;

  parser_ = std::make_shared<Parser>();
  eva_ = std::make_shared<Evaluator>();
}

void Executor::setup(std::string file,
                     const std::shared_ptr<ExecuteConfig> ecfg) {
  exe_config_ = ecfg;
  jobLimitCheck();
  clusterLimitCheck();

  parser_->parse(file);
  eva_res_.case_path = file;
  VLOG(4) << "param check.";
  paramCheck();  // op oriented check

  VLOG(4) << "Create input(/output) tensors.";
  createTensors();

  VLOG(4) << "Host malloc.";
  hostMalloc();

  if (parser_->device() == CPU) {
    if (!mluOnlyFast()) {
      VLOG(4) << "Host malloc (for cpu compute).";
      baselineInputMalloc();
      VLOG(4) << "Init data (random data for cpu compute).";
      initBaselineInput();  // init fp32 cpu data
      VLOG(4) << "Cast dtype (host fp32 -> mlu X).";
      castIn();  // init host data(copy to host_data).}
    }
  } else {
    if (!mluOnlyFast()) {
      flag_quant_mode_ = NO_QUANT;
      VLOG(4) << "Init data from prototxt.";
      initHostData();  // read data to host_data directly
      VLOG(4) << "Set quant param to tensor descs.";
      setQuantizedParam();  // set quant param}
    }
  }
  VLOG(4) << "Device malloc.";
  deviceMalloc();
  VLOG(4) << "Copy data from host to device.";
  copyIn();
  VLOG(4) << "switch to origin data buffer.";
  switchDataToOrigin();
  VLOG(4) << "Device malloc (for workspace).";
  workspaceMalloc();
}

void Executor::launch() {
  // comute for warm up
  VLOG(4) << "compute once for warm up.";
  GTEST_CHECK(CNRT_RET_SUCCESS ==
              cnrtPlaceNotifier(exe_context_->n_start, exe_context_->queue));
  compute();
  GTEST_CHECK(CNRT_RET_SUCCESS ==
              cnrtPlaceNotifier(exe_context_->n_stop, exe_context_->queue));
}

bool Executor::ready() {
  auto ret =
      cnrtQueueWaitNotifier(exe_context_->n_stop, exe_context_->queue, 0);
  if (CNRT_RET_ERR_NOT_READY == ret) {
    return false;
  } else if (CNRT_RET_SUCCESS == ret) {
    return true;
  } else {
    GTEST_CHECK(false,
                "Executor: This kernel call failed because error occurred.");
  }
}

void Executor::sync() { syncQueueAndGetHardwareTime(1); }

EvaluateResult Executor::teardown() {
  // comupte for perf test
  if (exe_config_->perf_repeat > 1) {
    VLOG(4) << "Mlu compute for perf test.";
    GTEST_CHECK(CNRT_RET_SUCCESS ==
                cnrtPlaceNotifier(exe_context_->n_start, exe_context_->queue));
    for (int i = 0; i < exe_config_->perf_repeat; ++i) {
      compute();
    }
    GTEST_CHECK(CNRT_RET_SUCCESS ==
                cnrtPlaceNotifier(exe_context_->n_stop, exe_context_->queue));
    syncQueueAndGetHardwareTime(exe_config_->perf_repeat);
    VLOG(4) << "End mlu compute.";
  }

  VLOG(4) << "Device free (for workspace).";
  workspaceFree();

  VLOG(4) << "Copy data from device to host.";
  copyOut();

  VLOG(4) << "Host malloc (for baseline output, fp32)";
  baselineOutputMalloc();
  if (parser_->device() == CPU) {
    VLOG(4) << "Begin cpu compute.";
    cpuCompute();
    // if out dtype is half, cast cpu data from float to half to float,
    // consistent with mlu.
    castHalfOuput();
    VLOG(4) << "End cpu compute.";
  } else {
    // baseline output
    VLOG(4) << "Read in baseline device outputs.";
    getBaselineOutput();  // read in baseline output
  }

  VLOG(4) << "Host malloc (for mlu output, fp32).";
  mluOutputMalloc();
  // copy output to mlu_fp32_output_
  VLOG(4) << "Cast dtype (mlu X -> host fp32).";
  castOut();

  // diff_preprocess
  diffPreprocess();

  // get criterions for each op
  getCriterionsUse();

  // diff
  VLOG(4) << "Calculate error between mlu and baseline device.";
  return evaluate();
}

EvaluateResult Executor::evaluate() {
  // get error func and threshold
  // it depends on func saved in pb.
  std::set<Evaluator::Criterion> criterions;
  bool common_threshold = parser_->common_threshold();

  if (common_threshold) {
    criterions = parser_->criterions(-1, criterions_use_);
    if (exe_config_->fixed_criterion) {
      // if exe_config_->fixed_criterion, we need compute diff1~diff3
      // if criterions already contain certain func, insert failed
      // set threshold as 0 for new diff func.
      criterions.insert(
          std::move(Evaluator::Criterion(Evaluator::DIFF1, 0.0, false)));
      criterions.insert(
          std::move(Evaluator::Criterion(Evaluator::DIFF2, 0.0, false)));
      criterions.insert(
          std::move(Evaluator::Criterion(Evaluator::DIFF3, 0.0, false)));
    }
  }
  auto threshold_use = parser_->threshold_use();
  bool skip_nan_n_inf = arch_skip_nan_inf.find(exe_context_->handle->arch) !=
                        arch_skip_nan_inf.end();

  // for error
  // for every output
  for (int i = 0; i < getOutputBlocks().size(); ++i) {
    if (getOutputBlocks()[i]->size == 0 || threshold_use[i] == 0) {
      continue;  // null output
    }
    MetaTensor *ts = parser_->output(i);

#if GTEST_DEBUG_ENABLE
    if (exe_config_->dump_data) {
      if (!ts->stride.empty()) {
        saveDataToFile("baseline_raw_" + ts->name, cpu_fp32_output_[i],
                       getCpuDtype(ts->dtype), ts->shape_count);
      }
    }
#endif

    if (!ts->stride.empty()) {
      VLOG(4) << "[WARNING] Executor: " << ts->name
              << " cpu ptr been strided_out.";
      size_t cpu_dtype_size = getSizeOfDataType(getCpuDtype(ts->dtype));
      void *temp = cpu_runtime_.allocate(ts->total_count * cpu_dtype_size);
      if (!flag_input_reuse_) {
        memset(temp, 0x0, ts->total_count * cpu_dtype_size);
      } else {
        // if input is reused, need init cpu_output by input data
        for (int j = 0; j < data_vector_.size(); j++) {
          if (data_vector_[j].is_output) {
            memcpy(temp, cpu_fp32_stride_input_[j],
                   ts->total_count * cpu_dtype_size);
            break;
          }
        }
      }
      tensor_stride_out(temp, cpu_fp32_output_[i], ts->shape, ts->stride,
                        cpu_dtype_size);
      cpu_runtime_.deallocate(cpu_fp32_output_[i]);
      cpu_fp32_output_[i] = (float *)temp;
      ts->cpu_ptr = (float *)temp;
    }

    // save output data to file
    if (exe_config_->dump_data) {
      saveDataToFile("baseline_" + ts->name, cpu_fp32_output_[i],
                     getCpuDtype(ts->dtype), ts->total_count);
      saveDataToFile("mlu_" + ts->name, mlu_fp32_output_[i],
                     getCpuDtype(ts->dtype), ts->total_count);
    }

    if (!common_threshold) {
      criterions = parser_->criterions(i, criterions_use_);
    }

    for (auto it = criterions.begin(); it != criterions.end(); ++it) {
      eva_->computeError(cpu_fp32_output_[i], mlu_fp32_output_[i],
                         ts->total_count, *it, ts->name, ts->dtype,
                         skip_nan_n_inf);
    }
  }

  getMluPerfInfo(&(eva_res_.mlu));

  eva_res_.errors = eva_->errors();
  eva_res_.is_passed = eva_->isPassed();
  eva_res_.what = std::move(eva_->what());

  // check baseline
  if (exe_config_->perf_baseline) {
    checkBaseline();  // update eva_res_
  }

  return eva_res_;
}

void Executor::getMluPerfInfo(PerfInfo *res) {
  // interface time
  double time = interface_timer_.duration(exe_config_->perf_repeat);
  res->interface_time = (time != 0) ? time : -1;

  // compute
  res->compute_force = getPeakComputeForce();
  if (parser_->node()->has_theory_compute_ops()) {
    res->theory_ops = parser_->node()->theory_compute_ops();
  } else {
    res->theory_ops = getTheoryOps();
  }

  // op / ( (latency(us) / 1000 / 1000) * PEAK_COMPUTE_FORCE(op/s) )
  res->compute_efficiency = eva_->computeEfficiency(
      res->theory_ops * 1000 * 1000, res->hardware_time, res->compute_force);

  // io
  res->io_bandwidth = getIoBandwidth();
  if (parser_->node()->has_theory_io_size()) {
    res->theory_io = parser_->node()->theory_io_size();
  } else {
    res->theory_io = getTheoryIoSize();
  }

  // io_size(byte) / ( (latency(us) / 1000 / 1000) * IO_BANDWIDTH(GB/s) )
  res->io_efficiency = eva_->computeEfficiency(
      res->theory_io /* * 1000 * 1000*/, res->hardware_time,
      res->io_bandwidth * 1000 /* * 1000 * 1000*/);

  res->workspace_size = eva_->getMluWorkspaceSize();
}

// call this func after getMluHardwareTime()
// deal with baseline check of perf test
void Executor::checkBaseline() {
  GTEST_CHECK(eva_res_.op_name != "",
              "Executor: missing op name, didn't set it. We need know it "
              "when get performance "
              "baseline threshold");

  double hw_time_base = 0;
  bool is_get_base_data = false;
  bool is_baseline_pass = true;
  double hw_time_mean =
      eva_res_.mlu.hardware_time;  // eva_->getMluHardwareTime();
  double scale_bound = 0;
  double threshold_absolute = 0;
  double threshold_relative = 0;
  double workspace_size = 0;

  std::string case_name = getTestCaseName(eva_res_.case_path);
  is_get_base_data = getXmlData(case_name, &hw_time_base, &workspace_size);
  if (is_get_base_data) {
    LOG(INFO) << "[Baseline]:hardware time of baseline is " << hw_time_base
              << " (us).";
    LOG(INFO) << "[Baseline]:workspace size of baseline is " << workspace_size
              << " (Bytes).";
  } else {  // new case
    LOG(INFO) << "[Baseline]:this case is new and do not have baseline data.";
  }
  if (is_get_base_data) {
    is_baseline_pass =
        updateBaselineStrategy(hw_time_mean, scale_bound, threshold_absolute,
                               threshold_relative, &hw_time_base);
    if (!is_baseline_pass) {
      LOG(ERROR) << "[Baseline]:scale_bound:" << scale_bound
                 << " ,threshold_absolute:" << threshold_absolute
                 << " ,threshold_relative:" << threshold_relative * 100 << "%";
      LOG(ERROR) << "[Baseline]:hardware time of baseline is " << hw_time_base
                 << " (us).";
      LOG(ERROR) << "[Baseline]:hardware time of this test is " << hw_time_mean
                 << " (us).";
    }
    if (eva_res_.mlu.workspace_size > workspace_size) {
      LOG(ERROR) << "[Baseline]:workspace size of baseline is "
                 << workspace_size << " (Bytes).";
      LOG(ERROR) << "[Baseline]:workspace size of this test is "
                 << eva_res_.mlu.workspace_size << " (Bytes).";
      is_baseline_pass = false;
      eva_res_.mlu.workspace_size = workspace_size;
    }
    if (!is_baseline_pass) {
      eva_res_.what.emplace_back(
          "The performance result exceed baseline "
          "threshold.");
    }
  } else {  // pass when new case
    hw_time_base = hw_time_mean;
    is_baseline_pass = true;
  }

  eva_res_.mlu.hardware_time_base = hw_time_base;
  eva_res_.is_passed = eva_res_.is_passed && is_baseline_pass;
}

// return op/cycle
// don't forget * 1GHz to get peak compute force
double Executor::getCtPeakComputeForce() {
  GTEST_CHECK(parser_->inputs().size() >= 1,
              "Executor: when get ct peak force, we need at least 1 input, "
              "but now input num is < 1.");

  // ct peak compute force
  auto cluster_num =
      mluop::runtime::getClusterLimitCapability(exe_context_->handle);
  auto core_num = exe_context_->handle->core_num_per_cluster;
  switch (parser_->inputs()[0].dtype) {
    case MLUOP_DTYPE_HALF:
    case MLUOP_DTYPE_INT16:
      return CT_PEAK_FLOAT16_COMPUTE_FORCE * cluster_num * core_num;
    case MLUOP_DTYPE_FLOAT:
    case MLUOP_DTYPE_INT32:
      return CT_PEAK_FLOAT32_COMPUTE_FORCE * cluster_num * core_num;
    default:
      return CT_PEAK_FLOAT32_COMPUTE_FORCE * cluster_num * core_num;
  }
}

double Executor::getLtPeakComputeForce() {
  GTEST_CHECK(parser_->inputs().size() >= 2,
              "Executor: when get lt peak force, we need at least 2 input, "
              "but now input num is < 2.");
  MetaTensor *mt1 = parser_->input(0);
  MetaTensor *mt2 = parser_->input(1);
  auto dtype1 = (mt1->oc_dt != MLUOP_DTYPE_INVALID) ? mt1->oc_dt : mt1->dtype;
  auto dtype2 = (mt2->oc_dt != MLUOP_DTYPE_INVALID) ? mt2->oc_dt : mt2->dtype;
  auto cluster_num =
      mluop::runtime::getClusterLimitCapability(exe_context_->handle);
  auto platform = exe_context_->handle->arch;
  auto core_num = exe_context_->handle->core_num_per_cluster;

  if (MLUOP_MLU220 == platform) {
    // dont have int4 + int4
    if (dtype1 == MLUOP_DTYPE_INT8 && dtype2 == MLUOP_DTYPE_INT8) {
      // int8 + int8
      return LT_PEAK_INT8_INT8_COMPUTE_FORCE_220 * cluster_num * core_num;
    } else if ((dtype1 == MLUOP_DTYPE_INT8 && dtype2 == MLUOP_DTYPE_INT16) ||
               (dtype1 == MLUOP_DTYPE_INT16 && dtype2 == MLUOP_DTYPE_INT8)) {
      // int16 + int8
      return LT_PEAK_INT16_INT8_COMPUTE_FORCE_220 * cluster_num * core_num;
    } else if (dtype1 == MLUOP_DTYPE_INT16 && dtype2 == MLUOP_DTYPE_INT16) {
      // int16 + int16
      return LT_PEAK_INT16_INT16_COMPUTE_FORCE_220 * cluster_num * core_num;
    }
  } else if (MLUOP_MLU270 == platform || MLUOP_MLU290 == platform) {
    if (dtype1 == MLUOP_DTYPE_INT8 && dtype2 == MLUOP_DTYPE_INT8) {
      // int8 + int8
      return LT_PEAK_INT8_INT8_COMPUTE_FORCE_270_290 * cluster_num * core_num;
    } else if ((dtype1 == MLUOP_DTYPE_INT8 && dtype2 == MLUOP_DTYPE_INT16) ||
               (dtype1 == MLUOP_DTYPE_INT16 && dtype2 == MLUOP_DTYPE_INT8)) {
      // int16 + int8
      return LT_PEAK_INT16_INT8_COMPUTE_FORCE_270_290 * cluster_num * core_num;
    } else if (dtype1 == MLUOP_DTYPE_INT16 && dtype2 == MLUOP_DTYPE_INT16) {
      // int16 + int16
      return LT_PEAK_INT16_INT16_COMPUTE_FORCE_270_290 * cluster_num * core_num;
    }
  } else if (dtype1 == MLUOP_DTYPE_HALF && dtype2 == MLUOP_DTYPE_HALF) {
    // fp16 + fp16
    return LT_PEAK_FP16_FP16_COMPUTE_FORCE * cluster_num * core_num;
  } else if ((dtype1 == MLUOP_DTYPE_FLOAT && dtype2 == MLUOP_DTYPE_HALF) ||
             (dtype1 == MLUOP_DTYPE_HALF && dtype2 == MLUOP_DTYPE_FLOAT)) {
    // fp16 + fp32
    return LT_PEAK_FP32_FP16_COMPUTE_FORCE * cluster_num * core_num;
  } else if (dtype1 == MLUOP_DTYPE_FLOAT && dtype2 == MLUOP_DTYPE_FLOAT) {
    // fp32 + fp32
    return LT_PEAK_FP32_FP32_COMPUTE_FORCE * cluster_num * core_num;
  }
  LOG(WARNING) << "Executor: got unsupported arch when get peak compute "
                  "force.";
  return -1;
}

// return op/s
double Executor::getPeakComputeForce() {
  GTEST_CHECK(eva_res_.op_name != "",
              "Executor: missing op name, didn't set it. We need know it "
              "when get peak compute force.");

  if (lt_op_set.find(eva_res_.op_name) != lt_op_set.end()) {
    return getLtPeakComputeForce() * 1000 * 1000 * 1000;  // * 1GHz // lt
  } else {
    return getCtPeakComputeForce() * 1000 * 1000 * 1000;  // * 1GHz
  }
}

int64_t Executor::getTheoryIoSize() {
  size_t total_size = 0;
  std::for_each(data_vector_.begin(), data_vector_.end(),
                [&](DataBlock b) { total_size += b.size; });
  VLOG(4) << "Executor: getTheoryIOs: " << total_size << " bytes";
  return total_size;
}

double Executor::getIoBandwidth() {
  double io_bandwidth = -1;
  auto platform = exe_context_->handle->arch;
  switch (platform) {
    case MLUOP_MLU220:
      io_bandwidth = IO_BANDWIDTH_MLU220;
      break;
    case MLUOP_MLU270:
      io_bandwidth = IO_BANDWIDTH_MLU270;
      break;
    case MLUOP_MLU290:
      io_bandwidth = IO_BANDWIDTH_MLU290;
      break;
    case MLUOP_MLU370:
      io_bandwidth = IO_BANDWIDTH_MLU370;
      break;
    default:
      LOG(WARNING) << "Executor: got unsupported arch when get io bandwidth.";
  }
  auto cluster_num =
      mluop::runtime::getClusterLimitCapability(exe_context_->handle);
  VLOG(4) << "Executor: io bandwidth is " << io_bandwidth << " GB/s";
  return io_bandwidth;
}

// create tensor desc
// and put them in 1 vector, but output tensor's is_output is true.
// and saved desc in MetaTensor's tensor and ctx->tensors
void Executor::createTensors() {
  auto create_tensor = [&](MetaTensor *mt, bool is_output) {
    if (unlikely((mt->null()))) {
      VLOG(4) << "Executor: skip creating tensor " << mt->name
              << ", set it as nullptr.";
      // if don't have this tensor, set it as nullptr;
      // push an desc as nullptr, and is_output marked as false.
      mt->tensor = nullptr;  //  keep meta tensor == tensor_desc_
      tensor_desc_.emplace_back(nullptr, is_output);
      return;
    }

    mluOpTensorDescriptor_t desc = nullptr;
    MLUOP_CHECK(mluOpCreateTensorDescriptor(&desc));
    if (mt->stride.empty()) {  // if no stride testing this api.
      MLUOP_CHECK(mluOpSetTensorDescriptor(desc, mt->layout, mt->dtype,
                                           mt->shape.size(), mt->shape.data()));
    } else {  // if has stride testing this api.
      MLUOP_CHECK(mluOpSetTensorDescriptorEx(desc, mt->layout, mt->dtype,
                                             mt->shape.size(), mt->shape.data(),
                                             mt->stride.data()));
    }
    MLUOP_CHECK(mluOpSetTensorDescriptorOnchipDataType(desc, mt->oc_dt));

    mt->tensor = desc;  //  keep meta tensor == tensor_desc_
    tensor_desc_.emplace_back(desc, is_output);
  };

  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    create_tensor(parser_->input(i), false);
  }

  if (flag_input_reuse_) {
    VLOG(4) << "Executor: skip creating output tensors, because of tensor "
               "reusing.";
    return;
  }

  // for all outputs
  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    create_tensor(parser_->output(i), true);
  }
}

void Executor::destroyTensors() noexcept {
  for (int i = 0; i < tensor_desc_.size(); ++i) {
    if (tensor_desc_[i].tensor != nullptr) {
      EXPECT_EQ(mluOpDestroyTensorDescriptor(tensor_desc_[i].tensor),
                MLUOP_STATUS_SUCCESS);
    }
  }
}

// -----------------------------------------------------------------
//   random(with stride)
//         |
//  malloc for cpu_fp32_in/out,mlu_fp32_out (without stride/only shape count)
//         |      (cast dtype and memcpy)
//         | ----------------------------->  host ptr (with
//         strided/total_count) |                                     |
//         (memcpy h2d) |                                  dev ptr | | (load
//         strided if need(in kernel))
//  cpu compute(only shape)                     mlu
//         |                                     | (store strided if need(in
//         kernel) |                                  dev ptr | | (memcpy d2h)
//         |                                  host ptr
//         |                                     | (cast dtype)
//    cpu output                            mlu output
//         | (strided if need)                   |
//         |                                     |
//         | <------------------------------------
//         |
//         |  (so dump input and output are strided, same as kernel)
//         v
//       diff
// -----------------------------------------------------------------

// malloc host ptr
// this ptr is for memcpy to mlu.
// if tensor has stride, this ptr size include stride
void Executor::hostMalloc() {
  auto initHostPtr = [this](MetaTensor *ts) {
    if (!mluOnlyFast()) {
      ts->host_ptr =
          cpu_runtime_.allocate(ts->total_count * ts->sizeof_dtype, ts->name);
      memset(ts->host_ptr, 0x0, ts->total_count * ts->sizeof_dtype);
      data_vector_.back().host_ptr = ts->host_ptr;
    }
  };

  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    MetaTensor *ts = parser_->input(i);
    data_vector_.emplace_back(ts, false);

    if (unlikely(ts->empty())) {
      continue;
    }

    ts->host_ptr =
        cpu_runtime_.allocate(ts->total_count * ts->sizeof_dtype, ts->name);
    memset(ts->host_ptr, 0x0, ts->total_count * ts->sizeof_dtype);
    data_vector_.back().host_ptr = ts->host_ptr;
  }

  // if input reuse, we don't create output.
  if (flag_input_reuse_) {
    // if reuse tensor don't need output, skip
    VLOG(4) << "Exeucutor: skip output host ptr malloc, because of tensor "
               "reusing.";
    return;
  }

  // create outputs
  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    MetaTensor *ts = parser_->output(i);
    data_vector_.emplace_back(ts, true);

    if (unlikely(ts->empty())) {
      continue;
    }

    ts->host_ptr =
        cpu_runtime_.allocate(ts->total_count * ts->sizeof_dtype, ts->name);
    memset(ts->host_ptr, 0x0, ts->total_count * ts->sizeof_dtype);
    data_vector_.back().host_ptr = ts->host_ptr;
  }
}

// malloc host ptr
void Executor::hostFree() noexcept {
  for (size_t i = 0; i < data_vector_.size(); ++i) {
    if (data_vector_[i].host_ptr != nullptr) {
      cpu_runtime_.deallocate(data_vector_[i].host_ptr);
      data_vector_[i].host_ptr = nullptr;
    }
  }
}

// call this function after host malloc
// read data from *pb.
// and write data to host ptr
// ONLY FOR NON-CPU MODE
void Executor::initHostData() {
  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    MetaTensor *ts = parser_->input(i);
    if (unlikely(ts->empty())) {
      continue;
    }

    // read data from prototxt.
    parser_->getInputTensorValue(i, data_vector_[i].host_ptr,
                                 data_vector_[i].count);
  }
}

// determine dtype of the cpu array for input/output tensor
mluOpDataType_t Executor::getCpuDtype(mluOpDataType_t tensor_dtype) {
  switch (tensor_dtype) {
    // DOUBLE data is still stored as DOUBLE dtype in cpu array;
    case MLUOP_DTYPE_DOUBLE: {
      return MLUOP_DTYPE_DOUBLE;
    } break;
    // each complex number is stored as a COMPLEX_FLOAT data in cpu array
    case MLUOP_DTYPE_COMPLEX_HALF:
    case MLUOP_DTYPE_COMPLEX_FLOAT: {
      return MLUOP_DTYPE_COMPLEX_FLOAT;
    } break;
    // the cpu array defaults to FLOAT dtype
    default:
      return MLUOP_DTYPE_FLOAT;
  }
}

// malloc for baseline input.
// actually only for cpu-mode, so it's fp32.
// and cast to mlu-dtype later, results will write to host_ptr.
void Executor::baselineInputMalloc() {
  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    MetaTensor *ts = parser_->input(i);
    if (unlikely(ts->empty())) {
      cpu_fp32_input_.push_back(nullptr);
      continue;
    }

    // malloc a ptr with stride, to get random value
    // if this tensor has stride, will stride_in in castIn()
    size_t cpu_dtype_size = getSizeOfDataType(getCpuDtype(ts->dtype));
    ts->cpu_ptr = (float *)cpu_runtime_.allocate(
        ts->total_count * cpu_dtype_size, ts->name);
    cpu_fp32_input_.push_back(ts->cpu_ptr);
    if (!ts->stride.empty() && flag_input_reuse_) {
      cpu_fp32_stride_input_.push_back(nullptr);
      cpu_fp32_stride_input_[i] =
          (float *)cpu_runtime_.allocate(ts->total_count * cpu_dtype_size);
    }
    memset(ts->cpu_ptr, 0x0, ts->total_count * cpu_dtype_size);
  }
}

// malloc for baseline output.
// and it's fp32.
// the output of cpu will write to this ptr, and then compute diff.
void Executor::baselineOutputMalloc() {
  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    MetaTensor *ts = parser_->output(i);
    if (unlikely(ts->empty())) {
      cpu_fp32_output_.push_back(nullptr);
      continue;
    }
    size_t cpu_dtype_size = getSizeOfDataType(getCpuDtype(ts->dtype));
    ts->cpu_ptr = (float *)cpu_runtime_.allocate(
        ts->shape_count * cpu_dtype_size, ts->name);
    cpu_fp32_output_.push_back(ts->cpu_ptr);
    memset(ts->cpu_ptr, 0x0, ts->shape_count * cpu_dtype_size);
  }
}

// read in output data from *pb
// and set it to cpu_fp32_output_
// ONLY FOR NON-CPU MODE,
// cpu mode will call cpuCompute() to compute output, don't need read from
// *pb. sometimes need cast mlu-dtype to fp32, if dtype in *pb is not fp32.
// use this cpu_fp32_output_ and mlu_fp32_output_ to compute diff.
void Executor::getBaselineOutput() {
  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    MetaTensor *ts = parser_->output(i);
    if (unlikely(ts->empty())) {
      continue;
    }

    // cast dtype to fp32
    void *temp = cpu_runtime_.allocate(ts->shape_count * ts->sizeof_dtype);
    parser_->getOutputTensorValue(i, temp, ts->shape_count);
    mluOpDataType_t cpu_dtype = getCpuDtype(ts->dtype);
    castDataOut(temp, ts->dtype, cpu_fp32_output_[i], cpu_dtype,
                ts->shape_count, NO_QUANT);
    cpu_runtime_.deallocate(temp);
  }
}

// free cpu_fp32_input_ and cpu_fp32_output_.
void Executor::baselineFree() noexcept {
  for (int i = 0; i < cpu_fp32_input_.size(); ++i) {
    cpu_runtime_.deallocate(cpu_fp32_input_[i]);
    cpu_fp32_input_[i] = nullptr;
  }
  for (int i = 0; i < cpu_fp32_output_.size(); ++i) {
    cpu_runtime_.deallocate(cpu_fp32_output_[i]);
    cpu_fp32_output_[i] = nullptr;
  }
  for (int i = 0; i < cpu_fp32_stride_input_.size(); ++i) {
    cpu_runtime_.deallocate(cpu_fp32_stride_input_[i]);
    cpu_fp32_stride_input_[i] = nullptr;
  }
}

// initialize cpu mode's input data.
// if *pb has random param or path, just generate random data or just read
// them. and put them on cpu_fp32_input_, because they are fp32. else,
// (value_i/value_f/value_h), read them in and cast to fp32.(if they are not
// fp32)
void Executor::initBaselineInput() {
  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    MetaTensor *ts = parser_->input(i);
    if (unlikely(ts->empty())) {
      continue;
    }

    mluOpDataType_t cpu_dtype = getCpuDtype(ts->dtype);
    size_t cpu_dtype_size = getSizeOfDataType(getCpuDtype(ts->dtype));
    if (needZeroInput()) {
      memset(cpu_fp32_input_[i], 0x0, ts->total_count * cpu_dtype_size);
      VLOG(4) << "input data have been set zero";
    } else if (VALUE_RANDOM == ts->value_type ||
               VALUE_PATH == ts->value_type) {  // generate random or read from
                                                // path
      parser_->getInputTensorValue(i, cpu_fp32_input_[i], ts->total_count);
    } else {
      void *temp = cpu_runtime_.allocate(ts->total_count * ts->sizeof_dtype);
      // read in data and (copy/ cast) to cpu_fp32_input_
      parser_->getInputTensorValue(i, temp, ts->total_count);
      castDataOut(temp,
                  ts->dtype,  // src data and dtype
                  cpu_fp32_input_[i],
                  cpu_dtype,        // dst data and dtype(fp32)
                  ts->total_count,  // count.
                  NO_QUANT, ts->position, ts->scale,
                  ts->offset);  // quant param.
      cpu_runtime_.deallocate(temp);
    }

#if GTEST_DEBUG_ENABLE
    if (exe_config_->dump_data) {
      saveDataToFile("baseline_raw_" + ts->name, cpu_fp32_input_[i],
                     getCpuDtype(ts->dtype), ts->total_count);
    }
#endif
  }
}

// malloc a memory for mlu output
// it's fp32, only for computing diff.
// the output of mlu (no matter what dtype) will cast to fp32, and saved here.
void Executor::mluOutputMalloc() {
  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    MetaTensor *ts = parser_->output(i);
    if (unlikely(ts->empty())) {
      mlu_fp32_output_.push_back(nullptr);
      continue;
    }

    size_t cpu_dtype_size = getSizeOfDataType(getCpuDtype(ts->dtype));
    void *temp =
        cpu_runtime_.allocate(ts->total_count * cpu_dtype_size, ts->name);
    mlu_fp32_output_.push_back((float *)temp);
    memset(temp, 0x0, ts->total_count * cpu_dtype_size);
  }
}

void Executor::mluOutputFree() noexcept {
  for (int i = 0; i < mlu_fp32_output_.size(); ++i) {
    // delete null is ok
    cpu_runtime_.deallocate(mlu_fp32_output_[i]);
    mlu_fp32_output_[i] = nullptr;
  }
}

// call this function after host_malloc
// malloc dev ptr, on mlu.
// memcpy data of host_ptr to this ptr later.
void Executor::deviceMalloc() {
  auto malloc_dev = [&](MetaTensor *mt, DataBlock *db) {
    if (unlikely(mt->empty())) {
      return;
    }

    void *dev_ptr = mlu_runtime_.allocate(db->size, mt->name);
    mt->dev_origin_ptr = dev_ptr;
    db->device_origin_ptr = dev_ptr;
  };

  auto malloc_dev_perf = [&](MetaTensor *mt, DataBlock *db) {
    if (unlikely(mt->empty())) {
      return;
    }
    void *dev_ptr = mlu_runtime_.allocate(db->size, mt->name);
    mt->dev_perf_ptr = dev_ptr;
    db->device_perf_ptr = dev_ptr;
  };

  if (flag_input_reuse_) {
    GTEST_CHECK(parser_->inputs().size(), data_vector_.size(),
                "Executor: tensor num in *pb is NOT equal to data_vector "
                "size, they should be equal.");
  } else {
    GTEST_CHECK(parser_->inputs().size() + parser_->outputs().size(),
                data_vector_.size(),
                "Executor: tensor num in *pb is NOT equal to data_vector "
                "size, they should be equal.");
  }

  // malloc for input.
  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    malloc_dev(parser_->input(i), &(data_vector_[i]));
  }
  // a copy memory for perf-repeat-mode.
  if (exe_config_->perf_repeat > 1) {
    for (size_t i = 0; i < parser_->inputs().size(); ++i) {
      malloc_dev_perf(parser_->input(i), &(data_vector_[i]));
    }
  }

  if (flag_input_reuse_) {
    return;
  }

  // malloc for output.
  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    malloc_dev(parser_->output(i),
               &(data_vector_[i + parser_->inputs().size()]));
  }
  // a copy memory for perf-repeat-mode.
  if (exe_config_->perf_repeat > 1) {
    for (size_t i = 0; i < parser_->outputs().size(); ++i) {
      malloc_dev_perf(parser_->output(i),
                      &(data_vector_[i + parser_->inputs().size()]));
    }
  }
}

// free device ptr
void Executor::deviceFree() noexcept {
  for (int i = 0; i < data_vector_.size(); ++i) {
    if (data_vector_[i].device_origin_ptr != nullptr) {
      EXPECT_EQ(mlu_runtime_.deallocate(data_vector_[i].device_origin_ptr),
                CNRT_RET_SUCCESS);
    }
    if (data_vector_[i].device_perf_ptr != nullptr) {
      EXPECT_EQ(mlu_runtime_.deallocate(data_vector_[i].device_perf_ptr),
                CNRT_RET_SUCCESS);
    }
  }
  EXPECT_EQ(mlu_runtime_.destroy(), CNRT_RET_SUCCESS);
}

// cast data from fp32 -> X
// and return p/s.
// if dequantify is true, reset src_data.
void Executor::castDataIn(float *src_data, mluOpDataType_t src_dtype,
                          void *dst_data, mluOpDataType_t dst_dtype,
                          size_t count, QuantMode quant_mode, int *pos,
                          float *sc, int *off, bool dequantify) {
  if (count == 0) {
    VLOG(4) << "skip castDataIn: count is zero";
    return;
  }
  if (src_dtype == dst_dtype) {
    memcpy(dst_data, src_data, count * getSizeOfDataType(src_dtype));
  } else if ((src_dtype == MLUOP_DTYPE_FLOAT &&
              dst_dtype == MLUOP_DTYPE_INT8) ||
             (src_dtype == MLUOP_DTYPE_FLOAT &&
              dst_dtype == MLUOP_DTYPE_INT16)) {
    auto in_dtype = cvtMluOpDtypeToCnrt(src_dtype);
    auto out_dtype = cvtMluOpDtypeToCnrt(dst_dtype);
    // need quant
    *pos = 0;
    *sc = 1.0;
    *off = 0;
    if (quant_mode == NO_QUANT) {
      cnrtQuantizedParam_t quant_param = nullptr;
      GTEST_CHECK(CNRT_RET_SUCCESS ==
                  cnrtCreateQuantizedParam(&quant_param, *pos, *sc, *off));
      GTEST_CHECK(CNRT_RET_SUCCESS == cnrtCastDataType(src_data, in_dtype,
                                                       dst_data, out_dtype,
                                                       count, quant_param));
      if (dequantify) {
        GTEST_CHECK(CNRT_RET_SUCCESS == cnrtCastDataType(dst_data, out_dtype,
                                                         src_data, in_dtype,
                                                         count, quant_param));
      }
      GTEST_CHECK(CNRT_RET_SUCCESS == cnrtDestroyQuantizedParam(quant_param));
    } else {
      if (dst_dtype == MLUOP_DTYPE_INT8) {
        MLUOP_CHECK(castFloat32ToFixed(src_data, (int8_t *)dst_data, count,
                                       *pos, *sc, *off));
        if (dequantify) {
          MLUOP_CHECK(castFixedToFloat32((int8_t *)dst_data, src_data, count,
                                         *pos, *sc, *off));
        }
      } else if (dst_dtype == MLUOP_DTYPE_INT16) {
        MLUOP_CHECK(castFloat32ToFixed(src_data, (int16_t *)dst_data, count,
                                       *pos, *sc, *off));
        if (dequantify) {
          MLUOP_CHECK(castFixedToFloat32((int16_t *)dst_data, src_data, count,
                                         *pos, *sc, *off));
        }
      }
    }
  } else if ((src_dtype == MLUOP_DTYPE_FLOAT &&
              (dst_dtype == MLUOP_DTYPE_INT64 ||
               dst_dtype == MLUOP_DTYPE_UINT64 ||
               dst_dtype == MLUOP_DTYPE_INT32 ||
               dst_dtype == MLUOP_DTYPE_UINT32 ||
               dst_dtype == MLUOP_DTYPE_UINT16 ||
               dst_dtype == MLUOP_DTYPE_HALF ||
               dst_dtype == MLUOP_DTYPE_UINT8 ||
               dst_dtype == MLUOP_DTYPE_BOOL)) ||
             (src_dtype == MLUOP_DTYPE_COMPLEX_FLOAT &&
              dst_dtype == MLUOP_DTYPE_COMPLEX_HALF)) {
    arrayCastFloatAndNormal(src_data, src_dtype, dst_data, dst_dtype, count);
    if (dequantify) {
      arrayCastFloatAndNormal(dst_data, dst_dtype, src_data, src_dtype, count);
    }
  } else {
    GTEST_CHECK(false,
                "Executor: when cast fp32 to dtype, found unsupported dtype.");
  }
}

// set p/s/o, cast data from fp32 -> X
void Executor::castDataOut(void *src_data, mluOpDataType_t src_dtype,
                           float *dst_data, mluOpDataType_t dst_dtype,
                           size_t count, QuantMode quant_mode, int pos,
                           float sc, int off) {
  if (count == 0) {
    VLOG(4) << "skip castDataOut: count is zero";
    return;
  }
  if (src_dtype == dst_dtype) {
    memcpy(dst_data, src_data, count * getSizeOfDataType(src_dtype));
  } else if (src_dtype == MLUOP_DTYPE_COMPLEX_HALF &&
             dst_dtype == MLUOP_DTYPE_COMPLEX_FLOAT) {
    arrayCastFloatAndNormal(src_data, src_dtype, dst_data, dst_dtype, count);
  } else if ((src_dtype == MLUOP_DTYPE_INT8 &&
              dst_dtype == MLUOP_DTYPE_FLOAT) ||
             (src_dtype == MLUOP_DTYPE_INT16 &&
              dst_dtype == MLUOP_DTYPE_FLOAT)) {
    auto in_dtype = cvtMluOpDtypeToCnrt(src_dtype);
    auto out_dtype = cvtMluOpDtypeToCnrt(dst_dtype);
    // need quant
    if (flag_quant_mode_ == NO_QUANT) {
      cnrtQuantizedParam_t quant_param = nullptr;
      GTEST_CHECK(CNRT_RET_SUCCESS ==
                  cnrtCreateQuantizedParam(&quant_param, pos, sc, off));
      GTEST_CHECK(CNRT_RET_SUCCESS == cnrtCastDataType(src_data, in_dtype,
                                                       dst_data, out_dtype,
                                                       count, quant_param));
      GTEST_CHECK(CNRT_RET_SUCCESS == cnrtDestroyQuantizedParam(quant_param));
    } else {
      if (src_dtype == MLUOP_DTYPE_INT8) {
        MLUOP_CHECK(castFixedToFloat32((int8_t *)src_data, dst_data, count, pos,
                                       sc, off));
      } else if (src_dtype == MLUOP_DTYPE_INT16) {
        MLUOP_CHECK(castFixedToFloat32((int16_t *)src_data, dst_data, count,
                                       pos, sc, off));
      }
    }
  } else if (dst_dtype == MLUOP_DTYPE_FLOAT &&
             (src_dtype == MLUOP_DTYPE_HALF || src_dtype == MLUOP_DTYPE_BOOL ||
              src_dtype == MLUOP_DTYPE_INT32 ||
              src_dtype == MLUOP_DTYPE_INT64 ||
              src_dtype == MLUOP_DTYPE_UINT8 ||
              src_dtype == MLUOP_DTYPE_UINT16 ||
              src_dtype == MLUOP_DTYPE_UINT32 ||
              src_dtype == MLUOP_DTYPE_UINT64)) {
    arrayCastFloatAndNormal(src_data, src_dtype, dst_data, dst_dtype, count);
  } else if (src_dtype == MLUOP_DTYPE_UINT8 && dst_dtype == MLUOP_DTYPE_HALF) {
    auto in_dtype = cvtMluOpDtypeToCnrt(src_dtype);
    auto out_dtype = cvtMluOpDtypeToCnrt(dst_dtype);
    GTEST_CHECK(CNRT_RET_SUCCESS == cnrtCastDataType(src_data, in_dtype,
                                                     dst_data, out_dtype, count,
                                                     nullptr));
  } else {
    LOG(WARNING) << "Executor::castDataOut(): Cast"
                 << getNameOfDataType(src_dtype) << "to"
                 << getNameOfDataType(dst_dtype) << "is not supported";
    GTEST_CHECK(false,
                "Executor: when cast dtype to fp32, found unsupported dtype.");
  }
}

// only for cpu-mode,
// cast fp32 to dtype (cpu_fp32_input_ -> host_ptr)
// cpu_fp32_input_ add stride and memcpy to host_ptr
void Executor::castIn() {
  for (size_t i = 0; i < parser_->inputs().size(); ++i) {
    MetaTensor *ts = parser_->input(i);
    if (unlikely(ts->empty())) {
      continue;
    }
    auto input_node = parser_->getProtoNode()->input(i);
    float *src_data = cpu_fp32_input_[i];
    void *dst_data = data_vector_[i].host_ptr;
    mluOpDataType_t cpu_dtype = getCpuDtype(ts->dtype);

    int position = input_node.has_position() ? input_node.position() : 0;
    int offset = input_node.has_offset() ? input_node.offset() : 0;
    float scale = input_node.has_scale() ? input_node.scale() : 1.0f;

    if (ts->oc_dt == MLUOP_DTYPE_INVALID || ts->oc_dt == ts->dtype ||
        flag_quant_mode_ == NO_QUANT) {
      // if no onchip p/s
      // just cast data from fp32 to dtype
      // then memcpy this to mlu
      castDataIn(src_data,
                 cpu_dtype,  // src data and dtype (fp32)
                 dst_data,
                 ts->dtype,        // dst data and dtype (in *pb)
                 ts->total_count,  // count
                 flag_quant_mode_, &position, &scale, &offset,
                 true);  // returned p/s
      MLUOP_CHECK(mluOpSetTensorDescriptorPositionAndScale(
          tensor_desc_[i].tensor, position, scale));
    } else {
      // if has onchip_dtype
      GTEST_CHECK((ts->dtype != MLUOP_DTYPE_DOUBLE) &&
                      (ts->dtype != MLUOP_DTYPE_COMPLEX_HALF) &&
                      (ts->dtype != MLUOP_DTYPE_COMPLEX_FLOAT),
                  "Executor::castIn():DOUBLE and COMPLEX dtypes are not "
                  "supported"
                  "when quantization is enabled!");
      // cast fp32 to onchip dtype to get p/s and dequantify fp32 data (let
      // cpu input == mlu input) and cast fp32 to offchip dtype then memcpy
      // this to mlu
      castDataIn(src_data,
                 MLUOP_DTYPE_FLOAT,  // src data
                 dst_data,
                 ts->dtype,        // dst data, memcpy this to mlu later.
                 ts->total_count,  // count
                 flag_quant_mode_, &position, &scale, &offset,
                 true);  // p/s, discarded.

      // get oc_dt's p/s and set to tensor.
      void *temp =
          cpu_runtime_.allocate(ts->total_count * getSizeOfDataType(ts->oc_dt));
      castDataIn(src_data,
                 MLUOP_DTYPE_FLOAT,  // src data
                 temp,
                 ts->oc_dt,        // dst data
                 ts->total_count,  // count
                 flag_quant_mode_, &position, &scale, &offset,
                 true);  // returned p/s, set to tensor.
      MLUOP_CHECK(mluOpSetTensorDescriptorPositionAndScale(
          tensor_desc_[i].tensor, position, scale));
      cpu_runtime_.deallocate(temp);
    }

    if (!ts->stride.empty()) {
      VLOG(4) << "Executor: " << ts->name << " host ptr been strided_out.";
      size_t cpu_dtype_size = getSizeOfDataType(getCpuDtype(ts->dtype));
      void *temp = cpu_runtime_.allocate(ts->shape_count * cpu_dtype_size);
      memset(temp, 0x0, ts->shape_count * cpu_dtype_size);
      if (flag_input_reuse_) {
        memcpy(cpu_fp32_stride_input_[i], cpu_fp32_input_[i],
               ts->total_count * cpu_dtype_size);
      }
      tensor_stride_in(temp, cpu_fp32_input_[i], ts->shape, ts->stride,
                       cpu_dtype_size);
      cpu_runtime_.deallocate(cpu_fp32_input_[i]);
      cpu_fp32_input_[i] = (float *)temp;
      ts->cpu_ptr = (float *)temp;
    }

    if (exe_config_->dump_data) {
      saveDataToFile("baseline_" + ts->name, cpu_fp32_input_[i],
                     getCpuDtype(ts->dtype), ts->shape_count);
    }
  }
}

// cast mlu's output to fp32
// and set them on mlu_fp32_output_
void Executor::castOut() {
  auto data_blocks = getOutputBlocks();
  auto output_num = data_blocks.size();
  // output data block num == output num
  // don't mark data block as output casually,
  // it's num must equals to output num in prototxt.
  GTEST_CHECK(output_num == parser_->outputs().size(),
              "Executor: output_num in *pb is not equal to num of tensor "
              "that marked as is_output = true.");

  for (size_t i = 0; i < parser_->outputs().size(); ++i) {
    MetaTensor *ts = parser_->output(i);
    if (unlikely(data_blocks[i]->count == 0)) {
      // if input reusing, here should check parser_->input().empty(), check
      // data_blocks[] instead. if not here should check
      // parser_->output().empty()
      continue;
    }

    // fp32 -> X
    void *src_data = data_blocks[i]->host_ptr;
    float *dst_data = mlu_fp32_output_[i];
    mluOpDataType_t cpu_dtype = getCpuDtype(ts->dtype);

    int position = 0;
    int offset = 0;
    float scale = 1.0f;
    castDataOut(src_data,
                ts->dtype,  // src data, mlu raw output
                dst_data,
                cpu_dtype,        // dst data, cast mlu output
                ts->total_count,  // count
                flag_quant_mode_, position, scale,
                offset);  // quant param.
  }
}

void Executor::switchDataToOrigin() {
  for (int i = 0; i < data_vector_.size(); ++i) {
    if (parser_->getMetaTensor(i).total_count != 0) {
      void *temp_ptr = parser_->getMetaTensor(i).dev_origin_ptr;
      parser_->getMetaTensor(i).dev_ptr = temp_ptr;
    } else {
      // don't have this tensor, it's null
    }
    data_vector_[i].device_ptr = parser_->getMetaTensor(i).dev_ptr;
  }
}

void Executor::switchDataToPerf() {
  for (int i = 0; i < data_vector_.size(); ++i) {
    if (parser_->getMetaTensor(i).total_count != 0) {
      void *temp_ptr = parser_->getMetaTensor(i).dev_perf_ptr;
      parser_->getMetaTensor(i).dev_ptr = temp_ptr;
    } else {
      // don't have this tensor, it's null
    }
    data_vector_[i].device_ptr = parser_->getMetaTensor(i).dev_ptr;
  }
}

void Executor::copyIn() {
  for (size_t i = 0; i < getInputBlocks().size(); ++i) {
    DataBlock *db = getInputBlocks()[i];

    // memcpy only for input
    if (unlikely(db->size == 0)) {
      VLOG(4) << "Executor: skip " << db->name << " memcpy device => host.";
      continue;
    }

    // memcpy host to dev
    if (!mluOnlyFast()) {
      auto t_a = std::chrono::system_clock::now();
      CNRT_CHECK(cnrtMemcpy(db->device_origin_ptr,
                            db->host_ptr,  // host to dev for compute
                            db->size, CNRT_MEM_TRANS_DIR_HOST2DEV));
      auto t_b = std::chrono::system_clock::now();
      auto dur =
          std::chrono::duration_cast<std::chrono::microseconds>(t_b - t_a);
      eva_res_.mlu.h2d_time += dur.count();
    }
    if (exe_config_->perf_repeat > 1 && !exe_config_->mlu_only) {
      // memcpy host to dev for perf test
      CNRT_CHECK(cnrtMemcpy(db->device_perf_ptr,
                            db->host_ptr,  // host to dev for perf repeat
                            db->size, CNRT_MEM_TRANS_DIR_HOST2DEV));
    }
    // for debug
    if (exe_config_->dump_data) {
      saveHexDataToFile("hex_" + db->name, db->host_ptr, db->dtype, db->count);
    }
  }
  for (size_t i = 0; i < getOutputBlocks().size(); ++i) {
    DataBlock *db = getOutputBlocks()[i];
    if (!db->stride.empty()) {
      // when output has stride param
      if (unlikely(db->size == 0)) {
        VLOG(4) << "Executor: skip " << db->name << " memcpy device => host.";
        continue;
      }
      // set zeros to dev
      auto t_a = std::chrono::system_clock::now();
      CNRT_CHECK(cnrtMemset(db->device_origin_ptr, 0, db->size));
      auto t_b = std::chrono::system_clock::now();
      auto dur =
          std::chrono::duration_cast<std::chrono::microseconds>(t_b - t_a);
      eva_res_.mlu.h2d_time += dur.count();

      if (exe_config_->perf_repeat > 1 && !exe_config_->mlu_only) {
        // set zeros to dev for perf test
        CNRT_CHECK(cnrtMemset(db->device_perf_ptr, 0, db->size));
      }
    }
  }
}

void Executor::copyOut() {
  for (int i = 0; i < getOutputBlocks().size(); ++i) {
    DataBlock *db = getOutputBlocks()[i];

    // memcpy only for output
    if (unlikely(db->size == 0)) {
      VLOG(4) << "Executor: skip " << db->name << " memcpy device => host.";
      continue;
    }

    // memcpy dev to host
    auto t_a = std::chrono::system_clock::now();
    GTEST_CHECK(CNRT_RET_SUCCESS == cnrtMemcpy(db->host_ptr,
                                               db->device_ptr,  // dev to host
                                               db->size,
                                               CNRT_MEM_TRANS_DIR_DEV2HOST));
    auto t_b = std::chrono::system_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t_b - t_a);
    eva_res_.mlu.d2h_time += dur.count();

    // for debug
    if (exe_config_->dump_data) {
      saveHexDataToFile("hex_" + db->name, db->host_ptr, db->dtype, db->count);
    }
  }
}

// after compute
void Executor::syncQueueAndGetHardwareTime(int repeat) {
  float hwtime = 0.0f;
  GTEST_CHECK(CNRT_RET_SUCCESS == cnrtQueueSync(exe_context_->queue));
  GTEST_CHECK(CNRT_RET_SUCCESS == cnrtNotifierDuration(exe_context_->n_start,
                                                       exe_context_->n_stop,
                                                       &hwtime));

  eva_res_.mlu.hardware_time = hwtime / repeat;
}

std::vector<DataBlock *> Executor::getInputBlocks() {
  std::vector<DataBlock *> temp;
  for (int i = 0; i < data_vector_.size(); ++i) {
    if (data_vector_[i].is_output == false) {
      temp.emplace_back(&data_vector_[i]);
    }
  }
  return temp;
}

std::vector<DataBlock *> Executor::getOutputBlocks() {
  std::vector<DataBlock *> temp;
  for (int i = 0; i < data_vector_.size(); ++i) {
    if (data_vector_[i].is_output == true) {
      temp.emplace_back(&data_vector_[i]);
    }
  }
  return temp;
}

void stride_map(void *dst,                           // dst ptr
                void *src,                           // src ptr
                const std::vector<int> &shape,       // shape
                const std::vector<int> &dst_stride,  // stride
                const std::vector<int> &src_stride,  // stride
                size_t dst_offset, size_t src_offset, size_t d,
                size_t sizeof_dtype, const size_t dst_max,
                const size_t src_max) {
  if (d == shape.size() - 1) {  // the last dim
    for (size_t i = 0; i < shape[d]; ++i) {
      size_t dst_idx = src_offset + i * src_stride[d];
      size_t src_idx = dst_offset + i * dst_stride[d];
      memcpy((char *)dst + dst_idx * sizeof_dtype,
             (char *)src + src_idx * sizeof_dtype, sizeof_dtype);
    }
  } else {
    for (size_t i = 0; i < shape[d]; ++i) {
      stride_map(dst, src, shape, dst_stride, src_stride,
                 dst_offset + i * dst_stride[d], src_offset + i * src_stride[d],
                 d + 1, sizeof_dtype, dst_max, src_max);
    }
  }
}

// src(strided) -> dst(shape)
// dst should malloc by shape_count
// src should malloc by stride_count
void Executor::tensor_stride_in(
    void *dst, void *src, const std::vector<int> &shape,
    const std::vector<int> &dst_stride,  // dst_stride
    size_t sizeof_dtype) {
  GTEST_CHECK(shape.size() == dst_stride.size(),
              "Executor: shape's size is not equal to stride's size.");

  size_t shape_total = std::accumulate(shape.begin(), shape.end(), (int)1,
                                       std::multiplies<int>());
  size_t stride_total = 1;
  for (size_t i = 0; i < shape.size(); ++i) {
    stride_total += (shape[i] - 1) * dst_stride[i];
  }

  std::vector<int> src_stride(shape.size());
  int stride_base = 1;
  for (ssize_t i = shape.size() - 1; i >= 0; --i) {
    src_stride[i] = stride_base;
    stride_base *= shape[i];
  }
  stride_map(dst, src, shape, dst_stride, src_stride, 0, 0, 0, sizeof_dtype,
             stride_total, shape_total);
}

// src(shape) -> dst(strided)
// dst should malloc by stride_count
// src should malloc by shape_count
void Executor::tensor_stride_out(
    void *dst, void *src, const std::vector<int> &shape,
    const std::vector<int> &src_stride,  // src_stride
    size_t sizeof_dtype) {
  GTEST_CHECK(shape.size() == src_stride.size(),
              "Executor: shape's size is not equal to stride's size.");

  size_t shape_total = std::accumulate(shape.begin(), shape.end(), (int)1,
                                       std::multiplies<int>());
  size_t stride_total = 1;
  for (size_t i = 0; i < shape.size(); ++i) {
    stride_total += (shape[i] - 1) * src_stride[i];
  }

  std::vector<int> dst_stride(shape.size());
  int stride_base = 1;
  for (ssize_t i = shape.size() - 1; i >= 0; --i) {
    dst_stride[i] = stride_base;
    stride_base *= shape[i];
  }
  stride_map(dst, src, shape, dst_stride, src_stride, 0, 0, 0, sizeof_dtype,
             shape_total, stride_total);
}

void Executor::castHalfOuput() {
  for (int i = 0; i < getOutputBlocks().size(); ++i) {
    if (getOutputBlocks()[i]->size == 0) {
      continue;  // null output
    }
    MetaTensor *ts = parser_->output(i);
    if (ts->dtype == MLUOP_DTYPE_HALF) {
      int16_t *half_data = (int16_t *)cpu_runtime_.allocate(
          ts->shape_count * getSizeOfDataType(ts->dtype));
      arrayCastFloatToHalf(half_data, cpu_fp32_output_[i], ts->shape_count);
      arrayCastHalfToFloat(cpu_fp32_output_[i], half_data, ts->shape_count);
    }
  }
}

void Executor::jobLimitCheck() {
  char *set_job_limit_ptr = getenv("MLUOP_SET_JOB_LIMIT_CAPABILITY");
  if (set_job_limit_ptr) {
    uint32_t set_job_limit = atoi(set_job_limit_ptr);
    VLOG(4) << "set job limit env successfully " << set_job_limit;
    uint32_t job_limit = mluop::runtime::getJobLimitCapability(handle_);
    VLOG(4) << "job_limit_before = " << job_limit;

    KernelClass cn_kernel_class = CN_KERNEL_CLASS_UNION4;
    switch (set_job_limit) {
      case 1:
        cn_kernel_class = CN_KERNEL_CLASS_UNION;
        break;
      case 2:
        cn_kernel_class = CN_KERNEL_CLASS_UNION2;
        break;
      case 3:
        cn_kernel_class = CN_KERNEL_CLASS_UNION4;
        break;
      case 4:
        cn_kernel_class = CN_KERNEL_CLASS_UNION8;
        break;
      case 5:
        cn_kernel_class = CN_KERNEL_CLASS_UNION16;
        break;
      case 6:
        // not use
        cn_kernel_class = CN_KERNEL_CLASS_BLOCK;
        break;
      case 7:
        // not use
        cn_kernel_class = CN_KERNEL_CLASS_NONE;
        break;
      default:
        LOG(WARNING) << "Executor: got unsupported job limit number."
                     << " Use default CN_KERNEL_CLASS_UNION4.";
    }
    setJobLimitCapability(cn_kernel_class);
    job_limit = mluop::runtime::getJobLimitCapability(handle_);
    VLOG(4) << "job_limit_after = " << job_limit;
  }
}

void Executor::clusterLimitCheck() {
  char *set_cluster_num_ptr = getenv("MLUOP_SET_CLUSTER_LIMIT_CAPABILITY");
  if (set_cluster_num_ptr) {
    uint32_t set_cluster_num = atoi(set_cluster_num_ptr);
    VLOG(4) << "set cluster num limit env successfully " << set_cluster_num;
    uint32_t union_number = mluop::runtime::getClusterLimitCapability(handle_);
    VLOG(4) << "union_number_before = " << union_number;
    // 255 is 8 cluster
    // 127 is 7 cluster
    // 63 is 6 cluster
    // 31 is 5 cluster
    // 15 is 4 cluster
    // 7 is 3 cluster
    // 3 is 2 cluster
    // 1 is 1 cluster
    std::vector<int> cluster_list{1, 3, 7, 15, 31, 63, 127, 255};
    std::vector<int>::iterator it;
    it = std::find(cluster_list.begin(), cluster_list.end(), set_cluster_num);
    if (it == cluster_list.end()) {
      set_cluster_num = 15;
      LOG(WARNING) << "Executor: got unsupported cluster limit number."
                   << " Use default 4 clusters. ";
    }
    setClusterLimitCapability(set_cluster_num);
    union_number = mluop::runtime::getClusterLimitCapability(handle_);
    VLOG(4) << "union_number_after = " << union_number;
  }
}

void Executor::setClusterLimitCapability(uint32_t cluster_limit) {
  CNctxConfigParam ctx_conf_param, check_param;
  ctx_conf_param.visibleCluster = cluster_limit;
  CNcontext ctx;
  (void)cnCtxGetCurrent(&ctx);
  int ret;
  ret =
      cnSetCtxConfigParam(ctx, CN_CTX_CONFIG_VISIBLE_CLUSTER, &ctx_conf_param);
  if (!ret) {
    cnGetCtxConfigParam(ctx, CN_CTX_CONFIG_VISIBLE_CLUSTER, &check_param);
    CHECK_EQ(check_param.visibleCluster, ctx_conf_param.visibleCluster);
  }
  handle_->capability_cluster_num = ctx_conf_param.visibleCluster;
}

void Executor::setJobLimitCapability(KernelClass kernel_class) {
  CNctxConfigParam ctx_conf_param, check_param;
  ctx_conf_param.unionLimit = kernel_class;
  CNcontext ctx;
  (void)cnCtxGetCurrent(&ctx);
  int ret;
  ret = cnSetCtxConfigParam(ctx, CN_CTX_CONFIG_UNION_LIMIT, &ctx_conf_param);
  if (!ret) {
    cnGetCtxConfigParam(ctx, CN_CTX_CONFIG_UNION_LIMIT, &check_param);
    CHECK_EQ(check_param.unionLimit, ctx_conf_param.unionLimit);
  }
  handle_->capability_job_limit = (int32_t)ctx_conf_param.unionLimit;
}

bool Executor::needZeroInput() {
  std::vector<std::string> bl_zero_input;
  std::string cur_op = parser_->getOpName();
  bl_zero_input = parser_->getBlOfZeroInput();
  auto it = find(bl_zero_input.begin(), bl_zero_input.end(), cur_op);
  if (exe_config_->zero_input) {
    if (it != bl_zero_input.end()) {
      VLOG(4) << cur_op << " not support zero input mode,"
              << " set input data as usual.";
      return false;
    }
    return true;
  }
  return false;
}

bool Executor::mluOnlyFast() {
  std::vector<std::string> bl_mlu_only_fast;
  std::string cur_op = parser_->getOpName();
  bl_mlu_only_fast = parser_->getBlOfMluOnlyFast();
  auto it = find(bl_mlu_only_fast.begin(), bl_mlu_only_fast.end(), cur_op);
  if (it == bl_mlu_only_fast.end()) {
    VLOG(4) << cur_op << ", mluOnlyFast";
  }
  if (exe_config_->mlu_only && it == bl_mlu_only_fast.end()) {
    if (!needZeroInput()) {
      return true;
    }
  }
  return false;
}
}  // namespace mluoptest
