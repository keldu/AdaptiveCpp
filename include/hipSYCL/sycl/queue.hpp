/*
 * This file is part of hipSYCL, a SYCL implementation based on CUDA/HIP
 *
 * Copyright (c) 2018-2020 Aksel Alpay and contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef HIPSYCL_QUEUE_HPP
#define HIPSYCL_QUEUE_HPP

#include "hipSYCL/common/debug.hpp"
#include "hipSYCL/glue/error.hpp"
#include "hipSYCL/runtime/application.hpp"
#include "hipSYCL/runtime/error.hpp"
#include "hipSYCL/runtime/hints.hpp"

#include "types.hpp"
#include "exception.hpp"

#include "property.hpp"
#include "libkernel/backend.hpp"
#include "device.hpp"
#include "device_selector.hpp"
#include "context.hpp"
#include "event.hpp"
#include "handler.hpp"
#include "info/info.hpp"
#include "detail/function_set.hpp"

#include <exception>
#include <memory>
#include <mutex>

namespace hipsycl {
namespace sycl {

namespace detail {

template<typename, int, access::mode, access::target>
class automatic_placeholder_requirement_impl;

using queue_submission_hooks =
  function_set<sycl::handler&>;
using queue_submission_hooks_ptr = 
  shared_ptr_class<queue_submission_hooks>;

}


namespace property::command_group {

template<int Dim>
struct hipSYCL_prefer_group_size : public detail::property{
  hipSYCL_prefer_group_size(range<Dim> r)
  : size{r} {}

  range<Dim> size;
};

struct hipSYCL_retarget : public detail::property{
  hipSYCL_retarget(const device& d)
  : dev{d} {}

  sycl::device dev;
};

}


namespace property::queue {

class in_order : public detail::property
{};

class enable_profiling : public detail::property
{};

}


class queue : public detail::property_carrying_object
{

  template<typename, int, access::mode, access::target>
  friend class detail::automatic_placeholder_requirement_impl;

public:
  explicit queue(const property_list &propList = {})
      : queue{default_selector{},
              [](exception_list e) { glue::default_async_handler(e); },
              propList} {
    assert(_default_hints.has_hint<rt::hints::bind_to_device>());
  }

  explicit queue(const async_handler &asyncHandler,
                 const property_list &propList = {})
      : queue{default_selector{}, asyncHandler, propList} {
    assert(_default_hints.has_hint<rt::hints::bind_to_device>());
  }

  explicit queue(const device_selector &deviceSelector,
                 const property_list &propList = {})
      : detail::property_carrying_object{propList},
        _ctx{deviceSelector.select_device()} {

    _handler = _ctx._impl->handler;

    this->init(deviceSelector.select_device()._device_id);
  }

  explicit queue(const device_selector &deviceSelector,
                 const async_handler &asyncHandler,
                 const property_list &propList = {})
      : detail::property_carrying_object{propList},
        _ctx{deviceSelector.select_device(), asyncHandler}, _handler{asyncHandler} {

    this->init(deviceSelector.select_device()._device_id);
  }

  explicit queue(const device &syclDevice, const property_list &propList = {})
      : detail::property_carrying_object{propList}, _ctx{syclDevice} {

    _handler = _ctx._impl->handler;

    this->init(syclDevice._device_id);
  }

  explicit queue(const device &syclDevice, const async_handler &asyncHandler,
                 const property_list &propList = {})
      : detail::property_carrying_object{propList},
        _ctx{syclDevice, asyncHandler}, _handler{asyncHandler} {

    this->init(syclDevice._device_id);
  }

  explicit queue(const context &syclContext,
                 const device_selector &deviceSelector,
                 const property_list &propList = {})
      : detail::property_carrying_object{propList}, _ctx{syclContext} {

    _handler = _ctx._impl->handler;
    
    device dev = deviceSelector.select_device();

    if (!is_device_in_context(dev, syclContext))
      throw invalid_object_error{"queue: Device is not in context"};

    this->init(dev._device_id);
  }

  explicit queue(const context &syclContext,
                 const device_selector &deviceSelector,
                 const async_handler &asyncHandler,
                 const property_list &propList = {})
      : detail::property_carrying_object{propList}, _ctx{syclContext},
        _handler{asyncHandler} {

    device dev = deviceSelector.select_device();

    if (!is_device_in_context(dev, syclContext))
      throw invalid_object_error{"queue: Device is not in context"};

    this->init(dev._device_id);
  }

  ~queue() {
    this->throw_asynchronous();
  }


  context get_context() const {
    return _ctx;
  }

  device get_device() const {
    if (_default_hints.has_hint<rt::hints::bind_to_device>()) {
      rt::device_id id =
          _default_hints.get_hint<rt::hints::bind_to_device>()->get_device_id();
      return device{id};
    }
    return device{};
  }

  bool is_host() const { return get_device().is_host(); }
  bool is_in_order() const {
    return _is_in_order;
  }

  void wait() {
    rt::application::dag().flush_sync();
    rt::application::dag().wait();
  }

  void wait_and_throw() {
    this->wait();
    this->throw_asynchronous();
  }

  void throw_asynchronous() {
    glue::throw_asynchronous_errors(_handler);
  }

  template <info::queue param>
  typename info::param_traits<info::queue, param>::return_type get_info() const;


  template <typename T>
  event submit(const property_list& prop_list, T cgf) {
    std::lock_guard<std::mutex> lock{*_lock};

    rt::execution_hints hints = _default_hints;
    
    if(prop_list.has_property<property::command_group::hipSYCL_retarget>()) {

      rt::execution_hints custom_hints;

      rt::device_id dev = detail::extract_rt_device(
          prop_list.get_property<property::command_group::hipSYCL_retarget>()
              .dev);

      if(!detail::extract_context_devices(_ctx).contains_device(dev)) {
        HIPSYCL_DEBUG_WARNING
            << "queue: Warning: Retargeting operation for a device that is not "
               "part of the queue's context. This can cause terrible problems if the "
               "operation uses USM allocations that were allocated using the "
               "queue's context."
            << std::endl;
      }

      custom_hints.add_hint(
          rt::make_execution_hint<rt::hints::bind_to_device>(dev));
      
      hints.overwrite_with(custom_hints);
    }

    handler cgh{get_context(), _handler, hints};
    
    apply_preferred_group_size<1>(prop_list, cgh);
    apply_preferred_group_size<2>(prop_list, cgh);
    apply_preferred_group_size<3>(prop_list, cgh);

    this->get_hooks()->run_all(cgh);

    rt::dag_node_ptr node = execute_submission(cgf, cgh);
    
    return event{node, _handler};
  }


  template <typename T>
  event submit(T cgf) {
    return submit(property_list{}, cgf);
  }

  template <typename T>
  event submit(T cgf, const queue &secondaryQueue,
               const property_list &prop_list = {}) {
    try {

      size_t num_errors_begin =
          rt::application::get_runtime().errors().num_errors();

      event evt = submit(prop_list, cgf);
      // Flush so that we see any errors during submission
      rt::application::dag().flush_sync();

      size_t num_errors_end =
          rt::application::get_runtime().errors().num_errors();

      bool submission_failed = false;
      // TODO This approach fails if an async handler has consumed
      // the errors in the meantime
      if(num_errors_end != num_errors_begin) {
        // Need to check if there was a kernel error..
        rt::application::get_runtime().errors().for_each_error(
            [&](const rt::result &err) {
              if (!err.is_success()) {
                if (err.info().get_error_type() ==
                    rt::error_type::kernel_error) {
                  submission_failed = true;
                }
              }
            });
      }

      if(!submission_failed) {
        return evt;
      } else {
        return secondaryQueue.submit(prop_list, cgf);
      }
    }
    catch(exception&) {
      return secondaryQueue.submit(prop_list, cgf);
    }
  }

  friend bool operator==(const queue& lhs, const queue& rhs)
  { return lhs._default_hints == rhs._default_hints; }

  friend bool operator!=(const queue& lhs, const queue& rhs)
  { return !(lhs == rhs); }

  // ---- Queue shortcuts ------

  template <typename KernelName = class _unnamed_kernel, typename KernelType>
  event single_task(const KernelType &KernelFunc) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.single_task<KernelName>(KernelFunc);
    });
  }

  template <typename KernelName = class _unnamed_kernel, typename KernelType>
  event single_task(event dependency, const KernelType &KernelFunc) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.single_task<KernelName>(KernelFunc);
    });
  }

  template <typename KernelName = class _unnamed_kernel, typename KernelType>
  event single_task(const std::vector<event> &dependencies,
                    const KernelType &KernelFunc) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.single_task<KernelName>(KernelFunc);
    });
  }

  template <typename KernelName = class _unnamed_kernel, 
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(range<Dims> NumWorkItems, 
                     const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.parallel_for<KernelName>(NumWorkItems, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(range<Dims> NumWorkItems, event dependency,
                     const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.parallel_for<KernelName>(NumWorkItems, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(range<Dims> NumWorkItems,
                     const std::vector<event> &dependencies,
                     const ReductionsAndKernel& ... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.parallel_for<KernelName>(NumWorkItems, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(range<Dims> NumWorkItems, id<Dims> WorkItemOffset,
                     const ReductionsAndKernel& ... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.parallel_for<KernelName>(NumWorkItems, WorkItemOffset,
                                   redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(range<Dims> NumWorkItems, id<Dims> WorkItemOffset,
                     event dependency,
                     const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.parallel_for<KernelName>(NumWorkItems, WorkItemOffset,
                                   redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(range<Dims> NumWorkItems, id<Dims> WorkItemOffset,
                     const std::vector<event> &dependencies,
                     const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.parallel_for<KernelName>(NumWorkItems, WorkItemOffset,
                                   redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(nd_range<Dims> ExecutionRange,
                     const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.parallel_for<KernelName>(ExecutionRange, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(nd_range<Dims> ExecutionRange, event dependency,
                     const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.parallel_for<KernelName>(ExecutionRange, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int Dims>
  event parallel_for(nd_range<Dims> ExecutionRange,
                     const std::vector<event> &dependencies,
                     const ReductionsAndKernel& ... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.parallel_for<KernelName>(ExecutionRange, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int dimensions>
  event parallel(range<dimensions> numWorkGroups,
                range<dimensions> workGroupSize,
                const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.parallel<KernelName>(numWorkGroups, workGroupSize, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int dimensions>
  event parallel(range<dimensions> numWorkGroups,
                range<dimensions> workGroupSize, event dependency,
                const ReductionsAndKernel& ... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.parallel<KernelName>(numWorkGroups, workGroupSize, redu_kernel...);
    });
  }

  template <typename KernelName = class _unnamed_kernel,
            typename... ReductionsAndKernel, int dimensions>
  event parallel(range<dimensions> numWorkGroups,
                range<dimensions> workGroupSize,
                const std::vector<event> &dependencies,
                const ReductionsAndKernel &... redu_kernel) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.parallel<KernelName>(numWorkGroups, workGroupSize, redu_kernel...);
    });
  }

  event memcpy(void *dest, const void *src, std::size_t num_bytes) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.memcpy(dest, src, num_bytes);
    });
  }

  event memcpy(void *dest, const void *src, std::size_t num_bytes,
               event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.memcpy(dest, src, num_bytes);
    });
  }

  event memcpy(void *dest, const void *src, std::size_t num_bytes,
               const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.memcpy(dest, src, num_bytes);
    });
  }

  event memset(void *ptr, int value, std::size_t num_bytes) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.memset(ptr, value, num_bytes);
    });
  }

  event memset(void *ptr, int value, std::size_t num_bytes, event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.memset(ptr, value, num_bytes);
    });
  }

  event memset(void *ptr, int value, std::size_t num_bytes,
               const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.memset(ptr, value, num_bytes);
    });
  }

  template <class T>
  event fill(void *ptr, const T &pattern, std::size_t count) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.fill(ptr, pattern, count);
    });
  }

  template <class T>
  event fill(void *ptr, const T &pattern, std::size_t count, event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.fill(ptr, pattern, count);
    });
  }

  template <class T>
  event fill(void *ptr, const T &pattern, std::size_t count,
             const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.fill(ptr, pattern, count);
    });
  }

  event prefetch(const void *ptr, std::size_t num_bytes) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.prefetch(ptr, num_bytes);
    });
  }

  event prefetch(const void *ptr, std::size_t num_bytes, event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.prefetch(ptr, num_bytes);
    });
  }

  event prefetch(const void *ptr, std::size_t num_bytes,
                 const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.prefetch(ptr, num_bytes);
    });
  }

  event prefetch_host(const void *ptr, std::size_t num_bytes) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.prefetch_host(ptr, num_bytes);
    });
  }

  event prefetch_host(const void *ptr, std::size_t num_bytes, event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.prefetch_host(ptr, num_bytes);
    });
  }

  event prefetch_host(const void *ptr, std::size_t num_bytes,
                      const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.prefetch_host(ptr, num_bytes);
    });
  }

  event mem_advise(const void *addr, std::size_t num_bytes, int advice) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.mem_advise(addr, num_bytes, advice);
    });
  }

  event mem_advise(const void *addr, std::size_t num_bytes, int advice,
                   event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.mem_advise(addr, num_bytes, advice);
    });
  }

  event mem_advise(const void *addr, std::size_t num_bytes, int advice,
                   const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.mem_advise(addr, num_bytes, advice);
    });
  }

  template<class InteropFunction>
  event hipSYCL_enqueue_custom_operation(InteropFunction op) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.hipSYCL_enqueue_custom_operation(op);
    });
  }

  template <class InteropFunction>
  event hipSYCL_enqueue_custom_operation(InteropFunction op, event dependency) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependency);
      cgh.hipSYCL_enqueue_custom_operation(op);
    });
  }

  template <class InteropFunction>
  event
  hipSYCL_enqueue_custom_operation(InteropFunction op,
                                   const std::vector<event> &dependencies) {
    return this->submit([&](sycl::handler &cgh) {
      cgh.depends_on(dependencies);
      cgh.hipSYCL_enqueue_custom_operation(op);
    });
  }


private:
  template<int Dim>
  void apply_preferred_group_size(const property_list& prop_list, handler& cgh) {
    if(prop_list.has_property<property::command_group::hipSYCL_prefer_group_size<Dim>>()){
      sycl::range<Dim> preferred_group_size =
          prop_list
              .get_property<
                  property::command_group::hipSYCL_prefer_group_size<Dim>>()
              .size;
      cgh.set_preferred_group_size(preferred_group_size);
    }
  }

  template <class Cgf>
  rt::dag_node_ptr execute_submission(Cgf cgf, handler &cgh) {
    if (is_in_order()) {
      auto previous = _previous_submission.lock();
      if(previous)
        cgh.depends_on(event{previous, _handler});
    }
    
    cgf(cgh);

    rt::dag_node_ptr node = this->extract_dag_node(cgh);
    if (is_in_order()) {
      _previous_submission = node;
    }
    return node;
  }
      
  bool is_device_in_context(const device &dev, const context &ctx) const {    
    std::vector<device> devices = ctx.get_devices();
    for (const auto context_dev : devices) {
      if (context_dev == dev)
        return true;
    }
    return false;
  }

  rt::dag_node_ptr extract_dag_node(sycl::handler& cgh) {
  
    const std::vector<rt::dag_node_ptr>& dag_nodes =
      cgh.get_cg_nodes();

    if(dag_nodes.empty()) {
      HIPSYCL_DEBUG_ERROR
          << "queue: Command queue evaluation did not result in the creation "
             "of events. Are there operations inside the command group?"
          << std::endl;
      return nullptr;
    }
    if(dag_nodes.size() > 1) {
      HIPSYCL_DEBUG_ERROR
          << "queue: Multiple events returned from command group evaluation; "
             "multiple operations in a single command group is not SYCL "
             "conformant. Returning event to the last operation"
          << std::endl;
    }
    return dag_nodes.back();
  }


  void init(rt::device_id device_id) {
    _default_hints.add_hint(rt::make_execution_hint<rt::hints::bind_to_device>(device_id));
    if (this->has_property<property::queue::enable_profiling>()) {
      _default_hints.add_hint(rt::make_execution_hint<rt::hints::enable_profiling>());
    }

    _is_in_order = this->has_property<property::queue::in_order>();
    _lock = std::make_shared<std::mutex>();

    this->_hooks = detail::queue_submission_hooks_ptr{
          new detail::queue_submission_hooks{}};
  }


  detail::queue_submission_hooks_ptr get_hooks() const
  {
    return _hooks;
  }
  
  detail::queue_submission_hooks_ptr _hooks;

  rt::execution_hints _default_hints;
  context _ctx;
  async_handler _handler;
  bool _is_in_order;

  std::weak_ptr<rt::dag_node> _previous_submission;
  std::shared_ptr<std::mutex> _lock;
};

HIPSYCL_SPECIALIZE_GET_INFO(queue, context)
{
  return get_context();
}

HIPSYCL_SPECIALIZE_GET_INFO(queue, device)
{
  return get_device();
}

HIPSYCL_SPECIALIZE_GET_INFO(queue, reference_count)
{
  return 1;
}

namespace detail{


template<typename dataT, int dimensions, access::mode accessMode,
            access::target accessTarget>
class automatic_placeholder_requirement_impl
{
public:
  automatic_placeholder_requirement_impl(sycl::queue &q, 
      sycl::accessor<dataT, dimensions, accessMode, accessTarget,
                access::placeholder::true_t>* acc)
    : _acc{acc}, _is_required{false}, _hooks{q.get_hooks()}
  {
    acquire();
  }

  void reacquire()
  {
    if(!_is_required)
      acquire();
  }

  void release()
  {
    if(_is_required)
      _hooks->remove(_hook_id);
    _is_required = false;
  }

  ~automatic_placeholder_requirement_impl()
  {
    if(_is_required)
      release();
  }

  bool is_required() const { return _is_required; }
  
private:
  void acquire()
  {
    auto acc = _acc;
    _hook_id = _hooks->add([acc] (sycl::handler& cgh) mutable{
      cgh.require(*acc);
    });

    _is_required = true;
  }

  bool _is_required;

  sycl::accessor<dataT, dimensions, accessMode, accessTarget,
                                  access::placeholder::true_t>* _acc;

  std::size_t _hook_id;
  detail::queue_submission_hooks_ptr _hooks;
};

}

namespace vendor {
namespace hipsycl {

template<typename dataT, int dimensions, access::mode accessMode,
            access::target accessTarget>
class automatic_placeholder_requirement
{
public:
  using impl_type = detail::automatic_placeholder_requirement_impl<
    dataT,dimensions,accessMode,accessTarget>;

  automatic_placeholder_requirement(queue &q, 
      accessor<dataT, dimensions, accessMode, accessTarget,
                access::placeholder::true_t>& acc)
  {
    _impl = std::make_unique<impl_type>(q, &acc);
  }

  automatic_placeholder_requirement(std::unique_ptr<impl_type> impl)
  : _impl{std::move(impl)}
  {}

  void reacquire()
  {
    _impl->reacquire();
  }

  void release()
  {
    _impl->release();
  }

  bool is_required() const
  {
    return _impl->is_required();
  }

private:
  std::unique_ptr<impl_type> _impl;
};

template<typename dataT, int dimensions, access::mode accessMode,
            access::target accessTarget>
inline auto automatic_require(queue &q, 
    accessor<dataT, dimensions, accessMode, accessTarget,access::placeholder::true_t>& acc)
{
  using requirement_type = automatic_placeholder_requirement<
    dataT, dimensions, accessMode, accessTarget>;

  using impl_type = typename requirement_type::impl_type;

  return requirement_type{std::make_unique<impl_type>(q, &acc)};
}

} // hipsycl
} // vendor



}// namespace sycl
}// namespace hipsycl



#endif
