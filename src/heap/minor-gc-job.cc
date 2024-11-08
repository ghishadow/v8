// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/minor-gc-job.h"

#include <memory>

#include "src/base/platform/time.h"
#include "src/execution/isolate.h"
#include "src/execution/vm-state-inl.h"
#include "src/flags/flags.h"
#include "src/heap/heap-inl.h"
#include "src/heap/heap.h"
#include "src/init/v8.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {

class MinorGCJob::Task : public CancelableTask {
 public:
  Task(Isolate* isolate, MinorGCJob* job)
      : CancelableTask(isolate), isolate_(isolate), job_(job) {}

  // CancelableTask overrides.
  void RunInternal() override;

  Isolate* isolate() const { return isolate_; }

 private:
  Isolate* const isolate_;
  MinorGCJob* const job_;
};

size_t MinorGCJob::YoungGenerationTaskTriggerSize(Heap* heap) {
  size_t young_capacity = 0;
  if (v8_flags.sticky_mark_bits) {
    // TODO(333906585): Adjust parameters.
    young_capacity = heap->sticky_space()->Capacity() -
                     heap->sticky_space()->old_objects_size();
  } else {
    young_capacity = heap->new_space()->TotalCapacity();
  }
  return young_capacity * v8_flags.minor_gc_task_trigger / 100;
}

bool MinorGCJob::YoungGenerationSizeTaskTriggerReached(Heap* heap) {
  if (v8_flags.sticky_mark_bits) {
    return heap->sticky_space()->young_objects_size() >=
           YoungGenerationTaskTriggerSize(heap);
  } else {
    return heap->new_space()->Size() >= YoungGenerationTaskTriggerSize(heap);
  }
}

void MinorGCJob::ScheduleTask() {
  if (!v8_flags.minor_gc_task) return;
  if (current_task_id_ != CancelableTaskManager::kInvalidTaskId) return;
  if (heap_->IsTearingDown()) return;
  // A task should be scheduled when young generation size reaches the task
  // trigger, but may also occur before the trigger is reached. For example,
  // this method is called from the allocation observer for new space. The
  // observer step size is detemine based on the current task trigger. However,
  // due to refining allocated bytes after sweeping (allocated bytes after
  // sweeping may be less than live bytes during marking), new space size may
  // decrease while the observer step size remains the same.
  std::shared_ptr<v8::TaskRunner> taskrunner = heap_->GetForegroundTaskRunner();
  if (taskrunner->NonNestableTasksEnabled()) {
    std::unique_ptr<Task> task = std::make_unique<Task>(heap_->isolate(), this);
    current_task_id_ = task->id();
    taskrunner->PostNonNestableTask(std::move(task));
  }
}

void MinorGCJob::CancelTaskIfScheduled() {
  if (current_task_id_ == CancelableTaskManager::kInvalidTaskId) return;
  // The task may have ran and bailed out already if major incremental marking
  // was running, in which `TryAbort` will return `kTaskRemoved`.
  heap_->isolate()->cancelable_task_manager()->TryAbort(current_task_id_);
  current_task_id_ = CancelableTaskManager::kInvalidTaskId;
}

void MinorGCJob::Task::RunInternal() {
  VMState<GC> state(isolate());
  TRACE_EVENT_CALL_STATS_SCOPED(isolate(), "v8", "V8.MinorGCJob.Task");

  DCHECK_EQ(job_->current_task_id_, id());
  job_->current_task_id_ = CancelableTaskManager::kInvalidTaskId;

  Heap* heap = isolate()->heap();
  if (v8_flags.separate_gc_phases &&
      isolate()->heap()->incremental_marking()->IsMajorMarking()) {
    // Don't trigger a minor GC while major incremental marking is active.
    return;
  }

  heap->CollectGarbage(NEW_SPACE, GarbageCollectionReason::kTask);
}

}  // namespace internal
}  // namespace v8
