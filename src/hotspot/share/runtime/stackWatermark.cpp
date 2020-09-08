/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/stackWatermark.inline.hpp"
#include "runtime/thread.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/preserveException.hpp"

class StackWatermarkIterator : public CHeapObj<mtInternal> {
  JavaThread* _jt;
  uintptr_t _caller;
  uintptr_t _callee;
  StackFrameStream _frame_stream;
  StackWatermark& _owner;
  bool _is_done;

  void set_watermark(uintptr_t sp);
  RegisterMap& register_map();
  frame& current();
  void next();

public:
  StackWatermarkIterator(StackWatermark& owner);
  uintptr_t caller() const { return _caller; }
  uintptr_t callee() const { return _callee; }
  void process_one(void* context);
  void process_all(void* context);
  bool has_next() const;
};

void StackWatermarkIterator::set_watermark(uintptr_t sp) {
  if (!has_next()) {
    return;
  }

  if (_callee == 0) {
    _callee = sp;
  } else if (_caller == 0) {
    _caller = sp;
  } else {
    _callee = _caller;
    _caller = sp;
  }
}

// This class encapsulates various marks we need to deal with calling the
// frame iteration code from arbitrary points in the runtime. It is mostly
// due to problems that we might want to eventually clean up inside of the
// frame iteration code, such as creating random handles even though there
// is no safepoint to protect against, and fiddling around with exceptions.
class StackWatermarkProcessingMark {
  ResetNoHandleMark _rnhm;
  HandleMark _hm;
  PreserveExceptionMark _pem;
  ResourceMark _rm;

public:
  StackWatermarkProcessingMark(Thread* thread) :
      _rnhm(),
      _hm(thread),
      _pem(thread),
      _rm(thread) { }
};

void StackWatermarkIterator::process_one(void* context) {
  StackWatermarkProcessingMark swpm(Thread::current());
  while (has_next()) {
    frame f = current();
    uintptr_t sp = reinterpret_cast<uintptr_t>(f.sp());
    bool frame_has_barrier = StackWatermark::has_barrier(f);
    _owner.process(f, register_map(), context);
    next();
    if (frame_has_barrier) {
      set_watermark(sp);
      break;
    }
  }
}

void StackWatermarkIterator::process_all(void* context) {
  const uintptr_t frames_per_poll_gc = 5;

  ResourceMark rm;
  log_info(stackbarrier)("Processing whole stack for tid %d",
                         _jt->osthread()->thread_id());
  uint i = 0;
  while (has_next()) {
    frame f = current();
    uintptr_t sp = reinterpret_cast<uintptr_t>(f.sp());
    assert(sp >= _caller, "invariant");
    bool frame_has_barrier = StackWatermark::has_barrier(f);
    _owner.process(f, register_map(), context);
    next();
    if (frame_has_barrier) {
      set_watermark(sp);
      if (++i == frames_per_poll_gc) {
        // Yield every N frames so mutator can progress faster.
        i = 0;
        _owner.yield_processing();
      }
    }
  }
}

StackWatermarkIterator::StackWatermarkIterator(StackWatermark& owner) :
    _jt(owner._jt),
    _caller(0),
    _callee(0),
    _frame_stream(owner._jt, true /* update_registers */, false /* process_frames */),
    _owner(owner),
    _is_done(_frame_stream.is_done()) {
}

frame& StackWatermarkIterator::current() {
  return *_frame_stream.current();
}

RegisterMap& StackWatermarkIterator::register_map() {
  return *_frame_stream.register_map();
}

bool StackWatermarkIterator::has_next() const {
  return !_is_done;
}

void StackWatermarkIterator::next() {
  _frame_stream.next();
  _is_done = _frame_stream.is_done();
}

StackWatermark::StackWatermark(JavaThread* jt, StackWatermarkSet::Kind kind, uint32_t epoch) :
    _state(StackWatermarkState::create(epoch, true /* is_done */)),
    _watermark(0),
    _next(NULL),
    _jt(jt),
    _iterator(NULL),
    _lock(Mutex::tty - 1, "stack_watermark_lock", true, Mutex::_safepoint_check_never),
    _kind(kind) {
}

StackWatermark::~StackWatermark() {
  delete _iterator;
}

void StackWatermark::assert_is_frame_safe(frame f) {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  assert(is_frame_safe(f), "Frame must be safe");
}

// A frame is "safe" if it *and* its caller have been processed. This is the invariant
// that allows exposing a frame, and for that frame to directly access its caller frame
// without going through any hooks.
bool StackWatermark::is_frame_safe(frame f) {
  assert(_lock.owned_by_self(), "Must be locked");
  uint32_t state = Atomic::load(&_state);
  if (!iteration_started(state)) {
    return false;
  }
  if (iteration_completed(state)) {
    return true;
  }
  return reinterpret_cast<uintptr_t>(f.sp()) < _iterator->caller();
}

void StackWatermark::start_iteration_impl(void* context) {
  log_info(stackbarrier)("Starting stack processing iteration for tid %d",
                         _jt->osthread()->thread_id());
  delete _iterator;
  if (_jt->has_last_Java_frame()) {
    _iterator = new StackWatermarkIterator(*this);
    // Always process three frames when starting an iteration.
    //
    // The three frames corresponds to:
    // 1) The callee frame
    // 2) The caller frame
    // This allows a callee to always be able to read state from its caller
    // without needing any special barriers.
    //
    // 3) An extra frame to deal with unwinding safepointing on the way out.
    // Sometimes, we also call into the runtime to on_unwind(), but then
    // hit a safepoint poll on the way out from the runtime.
    _iterator->process_one(context);
    _iterator->process_one(context);
    _iterator->process_one(context);
  } else {
    _iterator = NULL;
  }
  update_watermark();
}

void StackWatermark::yield_processing() {
  update_watermark();
  MutexUnlocker mul(&_lock, Mutex::_no_safepoint_check_flag);
}

void StackWatermark::update_watermark() {
  assert(_lock.owned_by_self(), "invariant");
  if (_iterator != NULL && _iterator->has_next()) {
    assert(_iterator->callee() != 0, "sanity");
    Atomic::release_store(&_watermark, _iterator->callee());
    Atomic::release_store(&_state, StackWatermarkState::create(epoch_id(), false /* is_done */)); // release watermark w.r.t. epoch
  } else {
    Atomic::release_store(&_watermark, uintptr_t(0)); // Release stack data modifications w.r.t. watermark
    Atomic::release_store(&_state, StackWatermarkState::create(epoch_id(), true /* is_done */)); // release watermark w.r.t. epoch
    log_info(stackbarrier)("Finished stack processing iteration for tid %d",
                           _jt->osthread()->thread_id());
  }
}

void StackWatermark::process_one() {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  if (!iteration_started()) {
    start_iteration_impl(NULL /* context */);
  } else if (!iteration_completed()) {
    _iterator->process_one(NULL /* context */);
    update_watermark();
  }
}

uintptr_t StackWatermark::watermark() {
  return Atomic::load_acquire(&_watermark);
}

uintptr_t StackWatermark::last_processed() {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  if (!iteration_started()) {
    // Stale state; no last processed
    return 0;
  }
  if (iteration_completed()) {
    // Already processed all; no last processed
    return 0;
  }
  return _iterator->caller();
}

bool StackWatermark::iteration_started() const {
  return iteration_started(Atomic::load(&_state));
}

bool StackWatermark::iteration_started_acquire() const {
  return iteration_started(Atomic::load_acquire(&_state));
}

bool StackWatermark::iteration_completed() const {
  return iteration_completed(Atomic::load(&_state));
}

bool StackWatermark::iteration_completed_acquire() const {
  return iteration_completed(Atomic::load_acquire(&_state));
}

void StackWatermark::start_iteration() {
  if (!iteration_started_acquire()) {
    MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
    if (!iteration_started()) {
      start_iteration_impl(NULL /* context */);
    }
  }
}

void StackWatermark::finish_iteration(void* context) {
  MutexLocker ml(&_lock, Mutex::_no_safepoint_check_flag);
  if (!iteration_started()) {
    start_iteration_impl(context);
  }
  if (!iteration_completed()) {
    _iterator->process_all(context);
    update_watermark();
  }
}
