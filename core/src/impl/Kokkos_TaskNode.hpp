/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Christian R. Trott (crtrott@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

// Experimental unified task-data parallel manycore LDRD

#ifndef KOKKOS_IMPL_TASKNODE_HPP
#define KOKKOS_IMPL_TASKNODE_HPP

#include <Kokkos_Macros.hpp>
#if defined( KOKKOS_ENABLE_TASKDAG )

#include <Kokkos_TaskScheduler_fwd.hpp>
#include <Kokkos_Core_fwd.hpp>

#include <Kokkos_PointerOwnership.hpp>

#include <impl/Kokkos_VLAEmulation.hpp>
#include <impl/Kokkos_LIFO.hpp>
#include <impl/Kokkos_LockfreeDeque.hpp>
#include <impl/Kokkos_EBO.hpp>
#include <Kokkos_Concepts.hpp>

#include <string>
#include <typeinfo>
#include <stdexcept>

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

namespace Kokkos {
namespace Impl {

enum TaskType : int16_t   { TaskTeam = 0 , TaskSingle = 1 , Aggregate = 2 };

//==============================================================================

/** Intrusive base class for things allocated with a Kokkos::MemoryPool
 *
 *  @warning Memory pools assume that the address of this class is the same
 *           as the address of the most derived type that was allocated to
 *           have the given size.  As a consequence, when interacting with
 *           multiple inheritance, this must always be the first base class
 *           of any derived class that uses it!
 *  @todo Consider inverting inheritance structure to avoid this problem?
 *
 *  @tparam CountType type of integer used to store the allocation size
 */
template <class CountType = int32_t>
class alignas(void*) PoolAllocatedObjectBase {
public:

  using pool_allocation_size_type = CountType;

private:

  pool_allocation_size_type m_alloc_size;

public:


  KOKKOS_INLINE_FUNCTION
  constexpr explicit PoolAllocatedObjectBase(pool_allocation_size_type allocation_size)
    : m_alloc_size(allocation_size)
  { }

  KOKKOS_INLINE_FUNCTION
  CountType get_allocation_size() const noexcept { return m_alloc_size; }

};

//==============================================================================


// TODO move this?
template <class CountType = int32_t>
class ReferenceCountedBase {
public:

  using reference_count_size_type = CountType;

private:

  reference_count_size_type m_ref_count = 0;

public:

  KOKKOS_INLINE_FUNCTION
  constexpr explicit
  ReferenceCountedBase(reference_count_size_type initial_reference_count)
    : m_ref_count(initial_reference_count)
  {
    // KOKKOS_EXPECTS(initial_reference_count > 0);
  }

  /** Decrement the reference count,
   *  and return true iff this decrement caused
   *  the reference count to become zero
   */
  KOKKOS_INLINE_FUNCTION
  bool decrement_and_check_reference_count()
  {
    // TODO memory order
    auto old_count = Kokkos::atomic_fetch_add(&m_ref_count, -1);

    KOKKOS_ASSERT(old_count > 0 && "reference count greater less than zero!");

    return (old_count == 1);
  }

  KOKKOS_INLINE_FUNCTION
  void increment_reference_count()
  {
    Kokkos::atomic_increment(&m_ref_count);
  }

};

template <class TaskQueueTraits, class SchedulingInfo>
class AggregateTask;

template <class TaskQueueTraits>
class RunnableTaskBase;

//==============================================================================

template <class TaskQueueTraits>
class TaskNode
  : public PoolAllocatedObjectBase<int32_t>, // size 4, must be first!
    public ReferenceCountedBase<int32_t>, // size 4
    public TaskQueueTraits::template intrusive_task_base_type<TaskNode<TaskQueueTraits>> // size 8+
{
public:

  using priority_type = int16_t;

private:

  using task_base_type = TaskNode<TaskQueueTraits>;
  using pool_allocated_base_type = PoolAllocatedObjectBase<int32_t>;
  using reference_counted_base_type = ReferenceCountedBase<int32_t>;
  using task_queue_traits = TaskQueueTraits;
  using waiting_queue_type =
    typename task_queue_traits::template waiting_queue_type<TaskNode>;

  waiting_queue_type m_wait_queue; // size 8+

  // TODO eliminate this???
  TaskQueueBase* m_ready_queue_base;

  TaskType m_task_type;  // size 2
  priority_type m_priority; // size 2

public:

  KOKKOS_INLINE_FUNCTION
  constexpr
  TaskNode(
    TaskType task_type,
    TaskPriority priority,
    TaskQueueBase* queue_base,
    reference_count_size_type initial_reference_count,
    pool_allocation_size_type allocation_size
  ) : pool_allocated_base_type(
        /* allocation_size = */ allocation_size
      ),
      reference_counted_base_type(
        /* initial_reference_count = */ initial_reference_count
      ),
      m_wait_queue(),
      m_task_type(task_type),
      m_priority(static_cast<priority_type>(priority)),
      m_ready_queue_base(queue_base)
  { }

  TaskNode() = delete;
  TaskNode(TaskNode const&) = delete;
  TaskNode(TaskNode&&) = delete;
  TaskNode& operator=(TaskNode const&) = delete;
  TaskNode& operator=(TaskNode&&) = delete;

  KOKKOS_INLINE_FUNCTION
  bool is_aggregate() const noexcept { return m_task_type == TaskType::Aggregate; }

  KOKKOS_INLINE_FUNCTION
  bool is_runnable() const noexcept { return m_task_type != TaskType::Aggregate; }

  KOKKOS_INLINE_FUNCTION
  bool is_single_runnable() const noexcept { return m_task_type == TaskType::TaskSingle; }

  KOKKOS_INLINE_FUNCTION
  bool is_team_runnable() const noexcept { return m_task_type == TaskType::TaskTeam; }

  KOKKOS_INLINE_FUNCTION
  TaskType get_task_type() const noexcept { return m_task_type; }

  KOKKOS_INLINE_FUNCTION
  RunnableTaskBase<TaskQueueTraits>&
  as_runnable_task() & {
    KOKKOS_EXPECTS(this->is_runnable());
    return static_cast<RunnableTaskBase<TaskQueueTraits>&>(*this);
  }

  KOKKOS_INLINE_FUNCTION
  RunnableTaskBase<TaskQueueTraits> const&
  as_runnable_task() const & {
    KOKKOS_EXPECTS(this->is_runnable());
    return static_cast<RunnableTaskBase<TaskQueueTraits> const&>(*this);
  }

  KOKKOS_INLINE_FUNCTION
  RunnableTaskBase<TaskQueueTraits>&&
  as_runnable_task() && {
    KOKKOS_EXPECTS(this->is_runnable());
    return static_cast<RunnableTaskBase<TaskQueueTraits>&&>(*this);
  }

  template <class SchedulingInfo>
  KOKKOS_INLINE_FUNCTION
  AggregateTask<TaskQueueTraits, SchedulingInfo>&
  as_aggregate() & {
    KOKKOS_EXPECTS(this->is_aggregate());
    return static_cast<AggregateTask<TaskQueueTraits, SchedulingInfo>&>(*this);
  }

  template <class SchedulingInfo>
  KOKKOS_INLINE_FUNCTION
  AggregateTask<TaskQueueTraits, SchedulingInfo> const&
  as_aggregate() const & {
    KOKKOS_EXPECTS(this->is_aggregate());
    return static_cast<AggregateTask<TaskQueueTraits, SchedulingInfo> const&>(*this);
  }

  template <class SchedulingInfo>
  KOKKOS_INLINE_FUNCTION
  AggregateTask<TaskQueueTraits, SchedulingInfo>&&
  as_aggregate() && {
    KOKKOS_EXPECTS(this->is_aggregate());
    return static_cast<AggregateTask<TaskQueueTraits, SchedulingInfo>&&>(*this);
  }

  KOKKOS_INLINE_FUNCTION
  bool try_add_waiting(task_base_type& depends_on_this) {
    return m_wait_queue.try_push(depends_on_this);
  }

  template <class Function>
  KOKKOS_INLINE_FUNCTION
  void consume_wait_queue(Function&& f) {
    KOKKOS_EXPECTS(not m_wait_queue.is_consumed());
    m_wait_queue.consume(std::forward<Function>(f));
  }

  KOKKOS_INLINE_FUNCTION
  bool wait_queue_is_consumed() const noexcept {
    // TODO memory order
    return m_wait_queue.is_consumed();
  }

  KOKKOS_INLINE_FUNCTION
  TaskQueueBase*
  ready_queue_base_ptr() const noexcept {
    return m_ready_queue_base;
  }

  KOKKOS_INLINE_FUNCTION
  void set_priority(TaskPriority priority) noexcept {
    KOKKOS_EXPECTS(!this->is_enqueued());
    m_priority = (priority_type)priority;
  }

  KOKKOS_INLINE_FUNCTION
  TaskPriority get_priority() const noexcept {
    return (TaskPriority)m_priority;
  }

};

//==============================================================================

template <class BaseClass, class SchedulingInfo>
struct SchedulingInfoStorage;

//==============================================================================

template <class BaseType, class SchedulingInfo>
class SchedulingInfoStorage
  : public BaseType, // must be first base class for allocation reasons!!!
    private NoUniqueAddressMemberEmulation<SchedulingInfo>
{

private:

  using base_t = BaseType;
  using task_scheduling_info_type = SchedulingInfo;

public:

  using base_t::base_t;

  KOKKOS_INLINE_FUNCTION
  task_scheduling_info_type& scheduling_info() &
  {
    return this->no_unique_address_data_member();
  }

  KOKKOS_INLINE_FUNCTION
  task_scheduling_info_type const& scheduling_info() const &
  {
    return this->no_unique_address_data_member();
  }

  KOKKOS_INLINE_FUNCTION
  task_scheduling_info_type&& scheduling_info() &&
  {
    return std::move(*this).no_unique_address_data_member();
  }

};


//==============================================================================

template <class TaskQueueTraits, class SchedulingInfo>
class AggregateTask final
  : public SchedulingInfoStorage<
      TaskNode<TaskQueueTraits>,
      SchedulingInfo
    >, // must be first base class for allocation reasons!!!
    public ObjectWithVLAEmulation<
      AggregateTask<TaskQueueTraits, SchedulingInfo>,
      OwningRawPtr<TaskNode<TaskQueueTraits>>
    >
{
private:

  using base_t = SchedulingInfoStorage<
    TaskNode<TaskQueueTraits>,
    SchedulingInfo
  >;
  using vla_base_t = ObjectWithVLAEmulation<
    AggregateTask<TaskQueueTraits, SchedulingInfo>,
    OwningRawPtr<TaskNode<TaskQueueTraits>>
  >;

  using task_base_type = TaskNode<TaskQueueTraits>;

public:

  using aggregate_task_type = AggregateTask; // concept marker

  template <class... Args>
    // requires std::is_constructible_v<base_t, Args&&...>
  KOKKOS_INLINE_FUNCTION
  constexpr explicit
  AggregateTask(
    int32_t aggregate_predecessor_count,
    Args&&... args
  ) : base_t(
        TaskType::Aggregate,
        TaskPriority::Regular, // all aggregates are regular priority
        std::forward<Args>(args)...
      ),
      vla_base_t(aggregate_predecessor_count)
  { }

  KOKKOS_INLINE_FUNCTION
  int32_t dependence_count() const { return this->n_vla_entries(); }

};

//KOKKOS_IMPL_IS_CONCEPT(aggregate_task);

//==============================================================================


template <class TaskQueueTraits>
class RunnableTaskBase
  : public TaskNode<TaskQueueTraits> // must be first base class for allocation reasons!!!
{
private:

  using base_t = TaskNode<TaskQueueTraits>;

public:

  using task_base_type = TaskNode<TaskQueueTraits>;
  using function_type = void(*)( task_base_type * , void * );
  using destroy_type = void(*)( task_base_type * );
  using runnable_task_type = RunnableTaskBase;

private:

  function_type m_apply;
  task_base_type* m_predecessor = nullptr;
  bool m_is_respawning = false;

public:

  template <class... Args>
    // requires std::is_constructible_v<base_t, Args&&...>
  KOKKOS_INLINE_FUNCTION
  constexpr explicit
  RunnableTaskBase(
    function_type apply_function_ptr,
    Args&&... args
  ) : base_t(std::forward<Args>(args)...),
      m_apply(apply_function_ptr)
  { }

  KOKKOS_INLINE_FUNCTION
  bool get_respawn_flag() const { return m_is_respawning; }

  KOKKOS_INLINE_FUNCTION
  void set_respawn_flag(bool value = true) { m_is_respawning = value; }

  KOKKOS_INLINE_FUNCTION
  bool has_predecessor() const { return m_predecessor != nullptr; }

  KOKKOS_INLINE_FUNCTION
  void clear_predecessor() { m_predecessor = nullptr; }

  template <class SchedulingInfo>
  KOKKOS_INLINE_FUNCTION
  SchedulingInfo&
  scheduling_info_as()
  {
    using info_storage_type = SchedulingInfoStorage<RunnableTaskBase, SchedulingInfo>;

    return static_cast<info_storage_type*>(this)->scheduling_info();
  }

  template <class SchedulingInfo>
  KOKKOS_INLINE_FUNCTION
  SchedulingInfo const&
  scheduling_info_as() const
  {
    using info_storage_type = SchedulingInfoStorage<RunnableTaskBase, SchedulingInfo>;

    return static_cast<info_storage_type const*>(this)->scheduling_info();
  }


  KOKKOS_INLINE_FUNCTION
  task_base_type& get_predecessor() const {
    KOKKOS_EXPECTS(m_predecessor != nullptr);
    return *m_predecessor;
  }

  KOKKOS_INLINE_FUNCTION
  void set_predecessor(task_base_type& predecessor)
  {
    KOKKOS_EXPECTS(m_predecessor == nullptr);
    // Increment the reference count so that predecessor doesn't go away
    // before this task is enqueued.
    // (should be memory order acquire)
    predecessor.increment_reference_count();
    m_predecessor = &predecessor;
  }

  template <class TeamMember>
  KOKKOS_INLINE_FUNCTION
  void run(TeamMember& member) {
    (*m_apply)(this, &member);
  }
};

//KOKKOS_IMPL_IS_CONCEPT(runnable_task);

//==============================================================================

template <class ResultType>
struct TaskResultStorage {
  ResultType m_value;
  ResultType& reference() { return m_value; }
};

template <>
struct TaskResultStorage<void> { };

//==============================================================================

template <
  class TaskQueueTraits,
  class Scheduler,
  class ResultType,
  class FunctorType
>
class RunnableTask
  : public SchedulingInfoStorage<
      RunnableTaskBase<TaskQueueTraits>,
      typename Scheduler::task_queue_type::task_scheduling_info_type
    >, // must be first base class
    public FunctorType,
    public TaskResultStorage<ResultType>
{
private:

  using base_t =
    SchedulingInfoStorage<
      RunnableTaskBase<TaskQueueTraits>,
      typename Scheduler::task_queue_type::task_scheduling_info_type
    >;
  using task_base_type = TaskNode<TaskQueueTraits>;
  using scheduler_type = Scheduler;
  using specialization = TaskQueueSpecialization<scheduler_type>;
  using member_type = typename specialization::member_type;
  using result_type = ResultType;
  using functor_type = FunctorType;
  using storage_base_type = TaskResultStorage<result_type>;

public:

  template <class... Args>
    // requires std::is_constructible_v<base_t, Args&&...>
  KOKKOS_INLINE_FUNCTION
  constexpr explicit
  RunnableTask(
    FunctorType&& functor,
    Args&&... args
  ) : base_t(
        std::forward<Args>(args)...
      ),
      functor_type(std::move(functor)),
      storage_base_type()
  { }

  KOKKOS_INLINE_FUNCTION
  ~RunnableTask() = delete;

  KOKKOS_INLINE_FUNCTION
  void update_scheduling_info(
    member_type& member
  ) {
    // TODO call a queue-specific hook here; for now, this info is already updated elsewhere
    // this->scheduling_info() = member.scheduler().scheduling_info();
  }

  KOKKOS_INLINE_FUNCTION
  void apply_functor(member_type* member, void*)
  {
    update_scheduling_info(*member);
    this->functor_type::operator()(*member);
  }

  template< typename T >
  KOKKOS_INLINE_FUNCTION
  void apply_functor(member_type* member, T* const result)
  {
    update_scheduling_info(*member);
    this->functor_type::operator()(*member, *result);
  }

  KOKKOS_FUNCTION static
  void destroy( task_base_type * root )
  {
    TaskResult<result_type>::destroy(root);
  }

  KOKKOS_FUNCTION static
  void apply(task_base_type* self, void* member_as_void)
  {
    auto* const task = static_cast<RunnableTask*>(self);
    auto* const member = reinterpret_cast<member_type*>(member_as_void);
    result_type* const result = TaskResult< result_type >::ptr( task );

    // Task may be serial or team.
    // If team then must synchronize before querying if respawn was requested.
    // If team then only one thread calls destructor.

    const bool only_one_thread =
#if defined(KOKKOS_ACTIVE_EXECUTION_MEMORY_SPACE_CUDA)
      0 == threadIdx.x && 0 == threadIdx.y ;
#else
      0 == member->team_rank();
#endif

    task->apply_functor(member, result);

    member->team_barrier();

    if ( only_one_thread && !(task->get_respawn_flag()) ) {
      // Did not respawn, destroy the functor to free memory.
      task->functor_type::~functor_type();
      // Cannot destroy and deallocate the task until its dependences
      // have been processed.
    }
  }

};

} /* namespace Impl */


} /* namespace Kokkos */

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

#endif /* #if defined( KOKKOS_ENABLE_TASKDAG ) */
#endif /* #ifndef KOKKOS_IMPL_TASKNODE_HPP */

