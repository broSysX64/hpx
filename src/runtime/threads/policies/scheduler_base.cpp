//  Copyright (c) 2007-2019 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/config.hpp>
#include <hpx/assertion.hpp>
#include <hpx/resource_partitioner/detail/partitioner.hpp>
#include <hpx/runtime/threads/policies/scheduler_base.hpp>
#include <hpx/runtime/threads/policies/scheduler_mode.hpp>
#include <hpx/runtime/threads/thread_init_data.hpp>
#include <hpx/runtime/threads/thread_pool_base.hpp>
#include <hpx/state.hpp>
#include <hpx/util/yield_while.hpp>
#include <hpx/util_fwd.hpp>
#if defined(HPX_HAVE_SCHEDULER_LOCAL_STORAGE)
#include <hpx/coroutines/detail/tss.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
    scheduler_base::scheduler_base(std::size_t num_threads,
        char const* description,
        thread_queue_init_parameters const& thread_queue_init,
        scheduler_mode mode)
      : suspend_mtxs_(num_threads)
      , suspend_conds_(num_threads)
      , pu_mtxs_(num_threads)
      , states_(num_threads)
      , description_(description)
      , thread_queue_init_(thread_queue_init)
      , parent_pool_(nullptr)
      , background_thread_count_(0)
    {
        set_scheduler_mode(mode);

#if defined(HPX_HAVE_THREAD_MANAGER_IDLE_BACKOFF)
        double max_time = thread_queue_init.max_idle_backoff_time_;

        wait_counts_.resize(num_threads);
        for (auto && data : wait_counts_)
        {
            data.data_.wait_count_ = 0;
            data.data_.max_idle_backoff_time_ = max_time;
        }
#endif

        for (std::size_t i = 0; i != num_threads; ++i)
            states_[i].store(state_initialized);
    }

    void scheduler_base::idle_callback(std::size_t num_thread)
    {
#if defined(HPX_HAVE_THREAD_MANAGER_IDLE_BACKOFF)
        if (mode_.data_.load(std::memory_order_relaxed) &
                policies::enable_idle_backoff)
        {
            // Put this thread to sleep for some time, additionally it gets
            // woken up on new work.

            idle_backoff_data& data = wait_counts_[num_thread].data_;

            // Exponential back-off with a maximum sleep time.
            double exponent = (std::min)(double(data.wait_count_),
                double(std::numeric_limits<double>::max_exponent - 1));

            std::chrono::milliseconds period(std::lround((std::min)(
                data.max_idle_backoff_time_, std::pow(2.0, exponent))));

            ++data.wait_count_;

            std::unique_lock<pu_mutex_type> l(mtx_);
            if (cond_.wait_for(l, period) == std::cv_status::no_timeout)
            {
                // reset counter if thread was woken up
                data.wait_count_ = 0;
            }
        }
#else
        (void)num_thread;
#endif
    }

    /// This function gets called by the thread-manager whenever new work
    /// has been added, allowing the scheduler to reactivate one or more of
    /// possibly idling OS threads
    void scheduler_base::do_some_work(std::size_t)
    {
#if defined(HPX_HAVE_THREAD_MANAGER_IDLE_BACKOFF)
        cond_.notify_all();
#endif
    }

    void scheduler_base::suspend(std::size_t num_thread)
    {
        HPX_ASSERT(num_thread < suspend_conds_.size());

        states_[num_thread].store(state_sleeping);
        std::unique_lock<pu_mutex_type> l(suspend_mtxs_[num_thread]);
        suspend_conds_[num_thread].wait(l);

        // Only set running if still in state_sleeping. Can be set with
        // non-blocking/locking functions to stopping or terminating, in
        // which case the state is left untouched.
        hpx::state expected = state_sleeping;
        states_[num_thread].compare_exchange_strong(expected, state_running);

        HPX_ASSERT(expected == state_sleeping ||
            expected == state_stopping || expected == state_terminating);
    }

    void scheduler_base::resume(std::size_t num_thread)
    {
        if (num_thread == std::size_t(-1))
        {
            for (std::condition_variable& c : suspend_conds_)
            {
                c.notify_one();
            }
        }
        else
        {
            HPX_ASSERT(num_thread < suspend_conds_.size());
            suspend_conds_[num_thread].notify_one();
        }
    }

    std::size_t scheduler_base::select_active_pu(
        std::unique_lock<pu_mutex_type>& l, std::size_t num_thread,
        bool allow_fallback)
    {
        if (mode_.data_.load(std::memory_order_relaxed) &
            threads::policies::enable_elasticity)
        {
            std::size_t states_size = states_.size();

            if (!allow_fallback)
            {
                // Try indefinitely as long as at least one thread is
                // available for scheduling. Increase allowed state if no
                // threads are available for scheduling.
                auto max_allowed_state = state_suspended;

                hpx::util::yield_while([this, states_size, &l, &num_thread,
                                           &max_allowed_state]() {
                    std::size_t num_allowed_threads = 0;

                    for (std::size_t offset = 0; offset < states_size; ++offset)
                    {
                        std::size_t num_thread_local =
                            (num_thread + offset) % states_size;

                        l = std::unique_lock<pu_mutex_type>(
                            pu_mtxs_[num_thread_local], std::try_to_lock);

                        if (l.owns_lock())
                        {
                            if (states_[num_thread_local] <= max_allowed_state)
                            {
                                num_thread = num_thread_local;
                                return false;
                            }

                            l.unlock();
                        }

                        if (states_[num_thread_local] <= max_allowed_state)
                        {
                            ++num_allowed_threads;
                        }
                    }

                    if (0 == num_allowed_threads)
                    {
                        if (max_allowed_state <= state_suspended)
                        {
                            max_allowed_state = state_sleeping;
                        }
                        else if (max_allowed_state <= state_sleeping)
                        {
                            max_allowed_state = state_stopping;
                        }
                        else
                        {
                            // All threads are terminating or stopped.
                            // Just return num_thread to avoid infinite
                            // loop.
                            return false;
                        }
                    }

                    // Yield after trying all pus, then try again
                    return true;
                });

                return num_thread;
            }

            // Try all pus only once if fallback is allowed
            HPX_ASSERT(num_thread != std::size_t(-1));
            for (std::size_t offset = 0; offset < states_size; ++offset)
            {
                std::size_t num_thread_local =
                    (num_thread + offset) % states_size;

                l = std::unique_lock<pu_mutex_type>(
                    pu_mtxs_[num_thread_local], std::try_to_lock);

                if (l.owns_lock() &&
                    states_[num_thread_local] <= state_suspended)
                {
                    return num_thread_local;
                }
            }
        }

        return num_thread;
    }

    // allow to access/manipulate states
    std::atomic<hpx::state>& scheduler_base::get_state(std::size_t num_thread)
    {
        HPX_ASSERT(num_thread < states_.size());
        return states_[num_thread];
    }
    std::atomic<hpx::state> const& scheduler_base::get_state(
        std::size_t num_thread) const
    {
        HPX_ASSERT(num_thread < states_.size());
        return states_[num_thread];
    }

    void scheduler_base::set_all_states(hpx::state s)
    {
        typedef std::atomic<hpx::state> state_type;
        for (state_type& state : states_)
        {
            state.store(s);
        }
    }

    void scheduler_base::set_all_states_at_least(hpx::state s)
    {
        typedef std::atomic<hpx::state> state_type;
        for (state_type& state : states_)
        {
            if (state < s)
            {
                state.store(s);
            }
        }
    }

    // return whether all states are at least at the given one
    bool scheduler_base::has_reached_state(hpx::state s) const
    {
        typedef std::atomic<hpx::state> state_type;
        for (state_type const& state : states_)
        {
            if (state.load(std::memory_order_relaxed) < s)
                return false;
        }
        return true;
    }

    bool scheduler_base::is_state(hpx::state s) const
    {
        typedef std::atomic<hpx::state> state_type;
        for (state_type const& state : states_)
        {
            if (state.load(std::memory_order_relaxed) != s)
                return false;
        }
        return true;
    }

    std::pair<hpx::state, hpx::state> scheduler_base::get_minmax_state() const
    {
        std::pair<hpx::state, hpx::state> result(
            last_valid_runtime_state, first_valid_runtime_state);

        typedef std::atomic<hpx::state> state_type;
        for (state_type const& state_iter : states_)
        {
            hpx::state s = state_iter.load();
            result.first = (std::min)(result.first, s);
            result.second = (std::max)(result.second, s);
        }

        return result;
    }

    // get/set scheduler mode
    void scheduler_base::set_scheduler_mode(scheduler_mode mode)
    {
        // distribute the same value across all cores
        mode_.data_.store(mode, std::memory_order_release);
        do_some_work(std::size_t(-1));
    }

    void scheduler_base::add_scheduler_mode(scheduler_mode mode)
    {
        // distribute the same value across all cores
        mode = scheduler_mode(get_scheduler_mode() | mode);
        set_scheduler_mode(mode);
    }

    void scheduler_base::remove_scheduler_mode(scheduler_mode mode)
    {
        mode = scheduler_mode(get_scheduler_mode() & ~mode);
        set_scheduler_mode(mode);
    }

    void scheduler_base::add_remove_scheduler_mode(
        scheduler_mode to_add_mode, scheduler_mode to_remove_mode)
    {
        scheduler_mode mode = scheduler_mode(
            (get_scheduler_mode() | to_add_mode) & ~to_remove_mode);
        set_scheduler_mode(mode);
    }

    void scheduler_base::update_scheduler_mode(scheduler_mode mode, bool set)
    {
        if (set) {
            add_scheduler_mode(mode);
        }
        else {
            remove_scheduler_mode(mode);
        }
    }

    ///////////////////////////////////////////////////////////////////////////
    std::size_t scheduler_base::domain_from_local_thread_index(std::size_t n)
    {
        auto &rp = resource::get_partitioner();
        auto const& topo = rp.get_topology();
        std::size_t global_id = local_to_global_thread_index(n);
        std::size_t pu_num = rp.get_pu_num(global_id);

        return topo.get_numa_node_number(pu_num);
    }

    // assumes queues use index 0..N-1 and correspond to the pool cores
    std::size_t scheduler_base::num_domains(const std::size_t workers)
    {
        auto &rp = resource::get_partitioner();
        auto const& topo = rp.get_topology();

        std::set<std::size_t> domains;
        for (std::size_t local_id = 0; local_id != workers; ++local_id)
        {
            std::size_t global_id = local_to_global_thread_index(local_id);
            std::size_t pu_num = rp.get_pu_num(global_id);
            std::size_t dom = topo.get_numa_node_number(pu_num);
            domains.insert(dom);
        }
        return domains.size();
    }

    // either threads in same domain, or not in same domain
    // depending on the predicate
    std::vector<std::size_t> scheduler_base::domain_threads(
        std::size_t local_id, const std::vector<std::size_t> &ts,
        std::function<bool(std::size_t, std::size_t)> pred)
    {
        std::vector<std::size_t> result;
        auto &rp = resource::get_partitioner();
        auto const& topo = rp.get_topology();
        std::size_t global_id = local_to_global_thread_index(local_id);
        std::size_t pu_num = rp.get_pu_num(global_id);
        std::size_t numa = topo.get_numa_node_number(pu_num);
        for (auto local_id : ts)
        {
            global_id = local_to_global_thread_index(local_id);
            pu_num = rp.get_pu_num(global_id);
            if (pred(numa, topo.get_numa_node_number(pu_num)))
            {
                result.push_back(local_id);
            }
        }
        return result;
    }

    ///////////////////////////////////////////////////////////////////////////
    std::int64_t scheduler_base::get_background_thread_count()
    {
        return background_thread_count_;
    }

    void scheduler_base::increment_background_thread_count()
    {
        ++background_thread_count_;
    }

    void scheduler_base::decrement_background_thread_count()
    {
        --background_thread_count_;
    }

#if defined(HPX_HAVE_SCHEDULER_LOCAL_STORAGE)
    coroutines::detail::tss_data_node* scheduler_base::find_tss_data(
        void const* key)
    {
        if (!thread_data_)
            return nullptr;
        return thread_data_->find(key);
    }

    void scheduler_base::add_new_tss_node(void const* key,
        std::shared_ptr<coroutines::detail::tss_cleanup_function>
            const& func, void* tss_data)
    {
        if (!thread_data_)
        {
            thread_data_ =
                std::make_shared<coroutines::detail::tss_storage>();
        }
        thread_data_->insert(key, func, tss_data);
    }

    void scheduler_base::erase_tss_node(void const* key, bool cleanup_existing)
    {
        if (thread_data_)
            thread_data_->erase(key, cleanup_existing);
    }

    void* scheduler_base::get_tss_data(void const* key)
    {
        if (coroutines::detail::tss_data_node* const current_node =
                find_tss_data(key))
        {
            return current_node->get_value();
        }
        return nullptr;
    }

    void scheduler_base::set_tss_data(void const* key,
        std::shared_ptr<coroutines::detail::tss_cleanup_function>
            const& func, void* tss_data, bool cleanup_existing)
    {
        if (coroutines::detail::tss_data_node* const current_node =
                find_tss_data(key))
        {
            if (func || (tss_data != 0))
                current_node->reinit(func, tss_data, cleanup_existing);
            else
                erase_tss_node(key, cleanup_existing);
        }
        else if(func || (tss_data != 0))
        {
            add_new_tss_node(key, func, tss_data);
        }
    }
#endif
}}}

