// Copyright 2019 Bytedance Inc. or its affiliates. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================

#include "scheduled_queue.h"
#include <algorithm>
#include "global.h"
#include "logging.h"

namespace byteps {
    namespace common {

        BytePSScheduledQueue::BytePSScheduledQueue(QueueType type) {

            if (type == REDUCE && BytePSGlobal::GetNccl()->IsSignalRoot()) {
                _is_scheduled = true;
            } else {
                _is_scheduled = false;
            }

            size_t credit_in_partition = BytePSGlobal::GetNccl()->GetGroupSize() + 1;
            if (getenv("BYTEPS_SCHEDULING_CREDIT")) {
                credit_in_partition = atoi(getenv("BYTEPS_SCHEDULING_CREDIT"));
            }
            if (!credit_in_partition) {
                _is_scheduled = false;
            }

            _qt = type;
            _credits = _is_scheduled
                       ? BytePSGlobal::GetPartitionBound() * credit_in_partition
                       : 34359738368;  // 32GB, basically disabling credit control
            _rt = nullptr;

            switch (_qt) {
                case REDUCE:
                    if (BytePSGlobal::GetNccl()->IsSignalRoot()) {
                        _rt = BytePSGlobal::GetReduceTable();
                    }
                    break;
                case PCIE_REDUCE:
                    if (BytePSGlobal::IsCrossPcieSwitch()) {
                        if (BytePSGlobal::GetCpuReducer()->isRoot()) {
                            _rt = BytePSGlobal::GetPcieReduceTable();
                        }
                    }
                    break;
                case PUSH:
                    if (BytePSGlobal::IsRootDevice()) {
                        _rt = BytePSGlobal::GetPushTable();
                    }
                    if(getenv("Z_BATCH_SIZE"))
                        batchsize = atoi(getenv("Z_BATCH_SIZE"));
                    if(getenv("MODEL")) {
                        if (!strcmp(getenv("MODEL"), "vgg19")) {
                            int tmp1[13] = {-1, 1, 13, 27, 37, 0, 77, 90, 103, 117, 130, 143, 156};
                            double tmp2[13] = {285.4, 196.2, 33.2, 0, 0, 53, 44, 64, 90, 74, 58, 15, 0}; // backward execution time
                            _init_pointer = 4;
                            for (int i = 0; i <= _init_pointer; i++) {
                                _grad_checkpoint[i] = tmp1[i];
                                _backward_exec[i] = tmp2[i];
                            }
                            begin_name = "DistributedGradientDescentOptimizer_Push_Pull/BytePSPushPull_gradients_resnet50_fc1000_BiasAdd_grad_tuple_control_dependency_1_0";
                            duration_ptr = 1;
                            duration = durations[duration_ptr];
                        }
                    }
                    _pointer = _init_pointer;
                    expected_priority = _grad_checkpoint[_pointer];
                    for (int i = 0; i < 13; i++) {
                        _backward_exec[i] *= (double)batchsize/64;
                    }
                    for (int i = 0; i < 13; i++) {
                        _backward_exec[i] *= B;
                    }
                    break;
                case COPYH2D:
                    if (!BytePSGlobal::IsRootDevice()) {
                        _rt = BytePSGlobal::GetCopyTable();
                    }
                    break;
                case BROADCAST:
                    if (BytePSGlobal::GetNccl()->IsSignalRoot()) {
                        _rt = BytePSGlobal::GetBroadcastTable();
                    }
                    break;
                case PULL:
                    break;
                default:
                    break;
            }
        }

        void BytePSScheduledQueue::addTask(std::shared_ptr <TensorTableEntry> entry) {
            std::lock_guard <std::mutex> lock(_mutex);
            if ((_qt == PUSH || _qt == PULL) && (entry->tensor_name).find("gradient") != (entry->tensor_name).npos) {
                _ms.insert(entry);
                _tensor_part[entry->priority * -1] = entry->total_partnum;
                if ((entry->tensor_name).find(begin_name) != (entry->tensor_name).npos) {
                    timer = getSystemTime();
                    duration_ptr = 0;
                    next_timer = timer + durations[duration_ptr];
                }
            } else {
                _sq.push_back(entry);
            }
            BPS_CHECK(entry->tensor_name != "");
            BPS_LOG(DEBUG) << "Queue " << LogStrings[_qt]
                           << " addTask: " << entry->tensor_name << " key: " << entry->key
                           << " rank: " << BytePSGlobal::GetLocalRank();
            return;
        }

        void BytePSScheduledQueue::recorderTs(std::shared_ptr <TensorTableEntry> task) {
            auto context = task->context;
            if (context->profile_flag) {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration);

                auto &queue_list = task->queue_list;
                BPS_CHECK_GE(queue_list.size(), 1);
                auto this_op = queue_list[0];

                BPSCommTime *ret = new BPSCommTime;
                ret->start_t = (long long) (us.count());
                ret->key = task->key;
                ret->type = this_op;
                context->part_comm_time[task->key][this_op].push(ret);
            }
        }

        struct isTargetPriority {
            int Priority;

            isTargetPriority(int priority) : Priority(priority) {}

            bool operator()(std::shared_ptr <TensorTableEntry> x) {
                return x->priority == Priority;
            }
        };

        std::multiset < std::shared_ptr < TensorTableEntry >> ::iterator BytePSScheduledQueue::findTask(int priority) {
            std::shared_ptr<TensorTableEntry> e(new TensorTableEntry);
            e->priority = priority;
            std::multiset < std::shared_ptr < TensorTableEntry >> ::iterator
            it = _ms.lower_bound(e);
            if (it == _ms.end()) {
                return it;
            } else if ((*it)->priority != priority) {
                return _ms.end();
            } else {
                BPS_CHECK_EQ((*it)->priority, priority);
                return it;
            }
        }

        std::shared_ptr <TensorTableEntry> BytePSScheduledQueue::getTask() {
            std::lock_guard <std::mutex> lock(_mutex);
            std::shared_ptr <TensorTableEntry> task;
            std::multiset < std::shared_ptr < TensorTableEntry >> ::iterator msit;
            if (_qt == PUSH && _ms.size() > 0) {
                long long now = getSystemTime();
//                BPS_LOG(INFO) << "now:" << now << " ,next_timer" << next_timer;
                if (now <= next_timer || duration_ptr == duration_ptr_len) {
                    msit = _ms.begin();
                    if (msit != _ms.end()) {
                        task = *msit;
//                        BPS_LOG(INFO) << "task:" << task->tensor_name << " ,size" << task->len << " ,dynamic:" << dynamic_size;
                        if (task -> len < dynamic_size || duration_ptr == duration_ptr_len) {
                            dynamic_size -= task -> len;
//                            BPS_LOG(INFO) << "start";
                            task->ready_event = nullptr;
                            recorderTs(task);
                            return task;
                        } else {
//                            BPS_LOG(INFO) << "no space left";
                            return nullptr;
                        }
                    } else {
                        return nullptr;
                    }
                } else {
                    dynamic_size = max_dynamic_size;
                    duration_ptr++;
                    if (duration_ptr < duration_ptr_len)
                        next_timer += durations[duration_ptr];
//                    BPS_LOG(INFO) << "reset:" << next_timer;
                    return nullptr;
                }
            } else {
                for (auto it = _sq.begin(); it != _sq.end(); ++it) {

                    if ((*it)->ready_event) {
                        if (!(*it)->ready_event->Ready()) {
                            continue;
                        }
                    }
                    if (_is_scheduled) {
                        if ((*it)->len > _credits)
                            continue;
                    }
                    if (_rt) {
                        if (!_rt->IsKeyReady((*it)->key)) {
                            continue;
                        }
                        _rt->ClearReadyCount((*it)->key);
                    }
                    task = *it;
                    if (_is_scheduled) {
                        _credits -= task->len;
                    }
                    _sq.erase(it);
                    BPS_CHECK(task->tensor_name != "");
                    BPS_LOG(DEBUG) << "Queue " << LogStrings[_qt]
                                   << " getTask: " << task->tensor_name << " key: " << task->key
                                   << " rank: " << BytePSGlobal::GetLocalRank();
                    task->ready_event = nullptr;
                    recorderTs(task);
                    return task;
                }
            }

            return nullptr;
        }

        std::shared_ptr <TensorTableEntry> BytePSScheduledQueue::getTask(uint64_t key) {
            BPS_CHECK(!_is_scheduled);
            std::lock_guard <std::mutex> lock(_mutex);
            std::shared_ptr <TensorTableEntry> task;
            for (auto it = _sq.begin(); it != _sq.end(); ++it) {
                if ((*it)->ready_event) {
                    BPS_CHECK((*it)->ready_event->Ready());
                }
                if ((*it)->key != (uint64_t) key) {
                    continue;
                }
                task = *it;
                _sq.erase(it);

                BPS_CHECK(task->tensor_name != "");
                BPS_LOG(DEBUG) << "Queue " << LogStrings[_qt]
                               << " getTask(key): " << task->tensor_name
                               << " key: " << task->key
                               << " rank: " << BytePSGlobal::GetLocalRank();
                task->ready_event = nullptr;
                recorderTs(task);
                return task;
            }
            return nullptr;
        }

        uint32_t BytePSScheduledQueue::pendingSize() {
            std::lock_guard <std::mutex> lock(_mutex);
            return _sq.size();
        }

        void BytePSScheduledQueue::reportFinish(std::shared_ptr < TensorTableEntry > task) {//
            std::lock_guard <std::mutex> lock(_mutex);
            if (_is_scheduled) {
                _credits += task -> len;
            }
            if (_qt == PUSH) {
                if (_meetzero) {
                    if (_dooropen < 11)
                        _dooropen++;
                }
            }
            return;
        }

    }  // namespace common
}  // namespace byteps
