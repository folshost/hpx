//  Copyright (c) 2007-2019 Hartmut Kaiser
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_SCHEDULING_LOCAL_WORKSTEALING_NOV_26_2019_0145PM)
#define HPX_THREADMANAGER_SCHEDULING_LOCAL_WORKSTEALING_NOV_26_2019_0145PM

#include <hpx/config.hpp>

#if defined(HPX_HAVE_LOCAL_WORKSTEALING_SCHEDULER)
#include <hpx/affinity.hpp>
#include <hpx/assertion.hpp>
#include <hpx/concurrency.hpp>
#include <hpx/errors.hpp>
#include <hpx/logging.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/policies/scheduler_base.hpp>
#include <hpx/runtime/threads/policies/thread_queue.hpp>
#include <hpx/runtime/threads/policies/thread_queue_init_parameters.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/synchronization.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <hpx/config/warnings_prefix.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies {
#ifdef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
    ///////////////////////////////////////////////////////////////////////////
    // We globally control whether to do minimal deadlock detection using this
    // global bool variable. It will be set once by the runtime configuration
    // startup code
    extern bool minimal_deadlock_detection;
#endif

    ///////////////////////////////////////////////////////////////////////////
#if defined(HPX_HAVE_CXX11_STD_ATOMIC_128BIT)
    using default_local_workstealing_scheduler_terminated_queue = lockfree_lifo;
#else
    using default_local_workstealing_scheduler_terminated_queue = lockfree_fifo;
#endif

    ///////////////////////////////////////////////////////////////////////////
    /// The local_workstealing_scheduler maintains exactly one queue of work
    /// items (threads) per OS thread, where this OS thread pulls its next work
    /// from.
    template <typename Mutex = std::mutex,
        typename PendingQueuing = lockfree_fifo,
        typename StagedQueuing = lockfree_fifo,
        typename TerminatedQueuing =
            default_local_workstealing_scheduler_terminated_queue>
    class HPX_EXPORT local_workstealing_scheduler : public scheduler_base
    {
    public:
        using has_periodic_maintenance = std::false_type;

        using thread_queue_type = thread_queue<Mutex, PendingQueuing,
            StagedQueuing, TerminatedQueuing>;

    private:
        ////////////////////////////////////////////////////////////////////////
        struct task_data
        {
            // core number this task data originated from
            std::uint16_t num_thread_;
            std::vector<thread_data*> tasks_;
        };

        ////////////////////////////////////////////////////////////////////////
        struct steal_request
        {
            enum class state : std::uint16_t
            {
                working = 0,
                idle = 2,
                failed = 4
            };

            steal_request()
              : channel_(nullptr)
              , victims_()
              , num_thread_(static_cast<std::uint16_t>(-1))
              , attempt_(0)
              , state_(state::failed)
            {
            }

            steal_request(std::size_t num_thread,
                lcos::local::channel_spsc<task_data>* channel,
                mask_cref_type victims, bool idle)
              : channel_(channel)
              , victims_(victims)
              , num_thread_(static_cast<std::uint16_t>(num_thread))
              , attempt_(0)
              , state_(idle ? state::idle : state::working)
            {
            }

            lcos::local::channel_spsc<task_data>* channel_;
            mask_type victims_;
            std::uint16_t num_thread_;
            std::uint16_t attempt_;
            state state_;
        };

        ////////////////////////////////////////////////////////////////////////
        struct scheduler_data
        {
            scheduler_data() noexcept
              : requested_(0)
              , num_thread_(static_cast<std::uint16_t>(-1))
#if defined(HPX_HAVE_WORKSTEALING_LAST_VICTIM)
              , last_victim_(static_cast<std::uint16_t>(-1))
#endif
              , victims_()
              , queue_(nullptr)
              , requests_(nullptr)
              , steal_requests_sent_(0)
              , steal_requests_received_(0)
              , steal_requests_discarded_(0)
            {
            }

            ~scheduler_data() = default;

            void init(std::size_t num_thread, std::size_t size,
                thread_queue_init_parameters const& init)
            {
                if (queue_ == nullptr)
                {
                    num_thread_ = static_cast<std::uint16_t>(num_thread);

                    queue_.reset(new thread_queue_type(num_thread, init));

                    requests_.reset(
                        new lcos::local::base_channel_mpsc<steal_request>(
                            size));

                    // max_num_to_steal_ is preset to half of the initial max
                    // queue length
                    tasks_.reset(new lcos::local::channel_spsc<task_data>(1));
                }
            }

            // the number of outstanding steal requests
            std::uint16_t requested_;

            // core number this scheduler data instance refers to
            std::uint16_t num_thread_;

#if defined(HPX_HAVE_WORKSTEALING_LAST_VICTIM)
            // core number the last stolen tasks originated from
            std::uint16_t last_victim_;
#endif
            // initial affinity mask for this core
            mask_type victims_;

            // queue for threads scheduled on this core
            std::unique_ptr<thread_queue_type> queue_;

            // channel for posting steal requests to this core
            std::unique_ptr<lcos::local::base_channel_mpsc<steal_request>>
                requests_;

            // one channel per steal request per core
            std::unique_ptr<lcos::local::channel_spsc<task_data>> tasks_;

            std::uint32_t steal_requests_sent_;
            std::uint32_t steal_requests_received_;
            std::uint32_t steal_requests_discarded_;
        };

    public:
        struct init_parameter
        {
            init_parameter(std::size_t num_queues,
                detail::affinity_data const& affinity_data,
                std::size_t numa_sensitive = 0,
                thread_queue_init_parameters const& thread_queue_init = {},
                char const* description = "local_workstealing_scheduler")
              : num_data_(num_queues)
              , thread_queue_init_(thread_queue_init)
              , affinity_data_(affinity_data)
              , description_(description)
            {
            }

            init_parameter(std::size_t num_queues,
                detail::affinity_data const& affinity_data,
                char const* description)
              : num_data_(num_queues)
              , thread_queue_init_()
              , affinity_data_(affinity_data)
              , description_(description)
            {
            }

            std::size_t num_data_;
            thread_queue_init_parameters thread_queue_init_;
            detail::affinity_data const& affinity_data_;
            char const* description_;
        };
        typedef init_parameter init_parameter_type;

        static unsigned int random_seed() noexcept
        {
            static std::random_device rd;
            return rd();
        }

        local_workstealing_scheduler(init_parameter_type const& init,
            bool deferred_initialization = true)
          : scheduler_base(
                init.num_data_, init.description_, init.thread_queue_init_)
          , data_(init.num_data_)
          , curr_queue_(0)
          , gen_(random_seed())
          , uniform_int_()
          , affinity_data_(init.affinity_data_)
        {
            if (!deferred_initialization)
            {
                HPX_ASSERT(init.num_data_ != 0);
                for (std::size_t i = 0; i < init.num_data_; ++i)
                {
                    data_[i].data_.init(
                        i, init.num_data_, this->thread_queue_init_);
                }
            }
        }

        ~local_workstealing_scheduler() override
        {
            int i = 0;
            (void) i;
        }

        static std::string get_scheduler_name()
        {
            return "local_workstealing_scheduler";
        }

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
        std::uint64_t get_creation_time(bool reset)
        {
            std::uint64_t time = 0;

            for (std::size_t i = 0; i != data_.size(); ++i)
                time += data_[i].data_.queue_->get_creation_time(reset);

            return time;
        }

        std::uint64_t get_cleanup_time(bool reset)
        {
            std::uint64_t time = 0;

            for (std::size_t i = 0; i != data_.size(); ++i)
                time += data_[i].data_.queue_->get_cleanup_time(reset);

            return time;
        }
#endif

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        std::int64_t get_num_pending_misses(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_pending_misses = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i != data_.size(); ++i)
                {
                    num_pending_misses +=
                        data_[i].data_.queue_->get_num_pending_misses(reset);
                }
                return num_pending_misses;
            }

            num_pending_misses +=
                data_[num_thread].data_.queue_->get_num_pending_misses(reset);
            return num_pending_misses;
        }

        std::int64_t get_num_pending_accesses(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_pending_accesses = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i != data_.size(); ++i)
                {
                    num_pending_accesses +=
                        data_[i].data_.queue_->get_num_pending_accesses(reset);
                }
                return num_pending_accesses;
            }

            num_pending_accesses +=
                data_[num_thread].data_.queue_->get_num_pending_accesses(reset);
            return num_pending_accesses;
        }

        std::int64_t get_num_stolen_from_pending(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i != data_.size(); ++i)
                {
                    num_stolen_threads +=
                        data_[i].data_.queue_->get_num_stolen_from_pending(
                            reset);
                }
                return num_stolen_threads;
            }

            num_stolen_threads +=
                data_[num_thread].data_.queue_->get_num_stolen_from_pending(
                    reset);
            return num_stolen_threads;
        }

        std::int64_t get_num_stolen_to_pending(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i != data_.size(); ++i)
                {
                    num_stolen_threads +=
                        data_[i].data_.queue_->get_num_stolen_to_pending(reset);
                }
                return num_stolen_threads;
            }

            num_stolen_threads +=
                data_[num_thread].data_.queue_->get_num_stolen_to_pending(
                    reset);
            return num_stolen_threads;
        }

        std::int64_t get_num_stolen_from_staged(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i != data_.size(); ++i)
                {
                    num_stolen_threads +=
                        data_[i].data_.queue_->get_num_stolen_from_staged(
                            reset);
                }
                return num_stolen_threads;
            }

            num_stolen_threads +=
                data_[num_thread].data_.queue_->get_num_stolen_from_staged(
                    reset);
            return num_stolen_threads;
        }

        std::int64_t get_num_stolen_to_staged(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;
            if (num_thread == std::size_t(-1))
            {
                for (std::size_t i = 0; i != data_.size(); ++i)
                {
                    num_stolen_threads +=
                        data_[i].data_.queue_->get_num_stolen_to_staged(reset);
                }
                return num_stolen_threads;
            }

            num_stolen_threads +=
                data_[num_thread].data_.queue_->get_num_stolen_to_staged(reset);
            return num_stolen_threads;
        }
#endif

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads() override
        {
            for (std::size_t i = 0; i != data_.size(); ++i)
                data_[i].data_.queue_->abort_all_suspended_threads();
        }

        ///////////////////////////////////////////////////////////////////////
        bool cleanup_terminated(bool delete_all) override
        {
            bool empty = true;
            for (std::size_t i = 0; i != data_.size(); ++i)
            {
                empty = data_[i].data_.queue_->cleanup_terminated(delete_all) &&
                    empty;
            }
            return empty;
        }

        bool cleanup_terminated(
            std::size_t num_thread, bool delete_all) override
        {
            return data_[num_thread].data_.queue_->cleanup_terminated(
                delete_all);
        }

        ///////////////////////////////////////////////////////////////////////
        // create a new thread and schedule it if the initial state is equal to
        // pending
        void create_thread(thread_init_data& data, thread_id_type* id,
            thread_state_enum initial_state, bool run_now,
            error_code& ec) override
        {
            // by default we always schedule new threads on our own queue
            std::size_t num_thread = std::size_t(-1);
            if (data.schedulehint.mode == thread_schedule_hint_mode_thread)
            {
                num_thread = data.schedulehint.hint;
            }

            std::size_t data_size = data_.size();
            if (std::size_t(-1) == num_thread)
            {
                num_thread = curr_queue_++ % data_size;
            }
            else if (num_thread >= data_size)
            {
                num_thread %= data_size;
            }

            std::unique_lock<pu_mutex_type> l;
            num_thread = select_active_pu(l, num_thread);

            data.schedulehint.mode = thread_schedule_hint_mode_thread;
            data.schedulehint.hint = static_cast<std::int16_t>(num_thread);

            HPX_ASSERT(num_thread < data_size);
            data_[num_thread].data_.queue_->create_thread(
                data, id, initial_state, run_now, ec);
        }

        // Retrieve the next viable steal request from our channel
        bool try_receiving_steal_request(
            scheduler_data& d, steal_request& req) noexcept
        {
            bool ret = d.requests_->get(&req);
            while (ret && req.state_ == steal_request::state::failed)
            {
                // forget the received steal request
                --data_[req.num_thread_].data_.requested_;
                HPX_ASSERT(data_[req.num_thread_].data_.requested_ == 0);

                // try to retrieve next steal request
                ret = d.requests_->get(&req);
            }

            // No special treatment for other states
            HPX_ASSERT(
                (ret && req.state_ != steal_request::state::failed) || !ret);

            return ret;
        }

        // Pass steal request on to another worker.
        // Returns true if we handled our own steal request.
        bool decline_or_forward_steal_request(
            scheduler_data& d, steal_request& req) noexcept
        {
            HPX_ASSERT(req.attempt_ < data_.size());

            if (req.num_thread_ == d.num_thread_)
            {
                // Steal request was either returned by another worker or
                // picked up by us.

                if (d.queue_->get_pending_queue_length(
                        std::memory_order_relaxed) > 0 ||
                    req.state_ == steal_request::state::idle)
                {
                    // we have work now, drop this steal request
                    ++d.steal_requests_discarded_;
                    --d.requested_;
                    HPX_ASSERT(d.requested_ == 0);
                }
                else
                {
                    // Continue circulating the steal request if it makes sense
                    req.attempt_ = 0;
                    req.state_ = steal_request::state::idle;
                    req.victims_ = d.victims_;

                    std::size_t victim = next_victim(d, req);
                    data_[victim].data_.requests_->set(std::move(req));

                    ++d.steal_requests_sent_;
                }

                return true;
            }

            // send this steal request on to the next (random) core
            ++req.attempt_;
            set(req.victims_, d.num_thread_);    // don't ask a core twice

            std::size_t victim = next_victim(d, req);
            data_[victim].data_.requests_->set(std::move(req));

            ++d.steal_requests_sent_;
            return false;
        }

        // decline_or_forward_all_steal_requests is only called when a worker
        // has nothing else to do but relay steal requests, which means the
        /// worker is idle.
        void decline_or_forward_all_steal_requests(scheduler_data& d) noexcept
        {
            steal_request req;
            while (try_receiving_steal_request(d, req))
            {
                ++d.steal_requests_received_;
                decline_or_forward_steal_request(d, req);
            }
        }

        // Handle a steal request by sending tasks in return or passing it on to
        // another worker. Returns true if the request was satisfied.
        bool handle_steal_request(
            scheduler_data& d, steal_request& req) noexcept
        {
            ++d.steal_requests_received_;

            if (req.num_thread_ == d.num_thread_)
            {
                // got back our own steal request.
                HPX_ASSERT(req.state_ != steal_request::state::failed);

                // Defer the decision to decline_steal_request
                decline_or_forward_steal_request(d, req);
                return false;
            }

            // Send at max HPX_MAX_STOLEN_TASKS from our queue to the requesting
            // core, but not more than half of the available tasks.
            std::size_t max_num_to_steal =
                d.queue_->get_pending_queue_length(std::memory_order_relaxed) /
                2;

            if (max_num_to_steal != 0)
            {
                task_data thrds;
                thrds.tasks_.reserve(max_num_to_steal);

                thread_data* thrd = nullptr;
                while (--max_num_to_steal != 0 &&
                    d.queue_->get_next_thread(thrd, true, true))
                {
                    d.queue_->increment_num_stolen_from_pending();
                    thrds.tasks_.push_back(thrd);
                }

                // we are ready to send at least one task
                if (!thrds.tasks_.empty())
                {
                    // send these tasks to the core that has sent the steal request
                    thrds.num_thread_ = d.num_thread_;
                    req.channel_->set(std::move(thrds));
                    return true;
                }
            }

            // There's nothing we can do with this steal request except pass
            // it on to a different worker
            decline_or_forward_steal_request(d, req);
            return false;
        }

        // Return the next thread to be executed, return false if none is
        // available
        bool get_next_thread(std::size_t num_thread, bool running,
            threads::thread_data*& thrd, bool enable_stealing) override
        {
            HPX_ASSERT(num_thread < data_.size());

            auto& d = data_[num_thread].data_;
            bool result = d.queue_->get_next_thread(thrd);

            d.queue_->increment_num_pending_accesses();
            if (enable_stealing && result)
            {
                // We found a task to run, however before running it we handle
                // steal requests (assuming that that there is more work left
                // that could be used to satisfy steal requests).

                steal_request req;
                while (try_receiving_steal_request(d, req))
                {
                    if (!handle_steal_request(d, req))
                        break;
                }
                return true;
            }

            d.queue_->increment_num_pending_misses();
            return false;
        }

        // Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd,
            threads::thread_schedule_hint schedulehint, bool allow_fallback,
            thread_priority priority = thread_priority_normal) override
        {
            std::size_t num_thread = std::size_t(-1);
            if (schedulehint.mode == thread_schedule_hint_mode_thread)
            {
                num_thread = schedulehint.hint;
            }
            else
            {
                allow_fallback = false;
            }

            std::size_t data_size = data_.size();
            if (std::size_t(-1) == num_thread)
            {
                num_thread = curr_queue_++ % data_size;
            }
            else if (num_thread >= data_size)
            {
                num_thread %= data_size;
            }

            std::unique_lock<pu_mutex_type> l;
            num_thread = select_active_pu(l, num_thread, allow_fallback);

            HPX_ASSERT(thrd->get_scheduler_base() == this);
            HPX_ASSERT(num_thread < data_size);

            data_[num_thread].data_.queue_->schedule_thread(thrd);
        }

        void schedule_thread_last(threads::thread_data* thrd,
            threads::thread_schedule_hint schedulehint, bool allow_fallback,
            thread_priority priority = thread_priority_normal) override
        {
            std::size_t num_thread = std::size_t(-1);
            if (schedulehint.mode == thread_schedule_hint_mode_thread)
            {
                num_thread = schedulehint.hint;
            }
            else
            {
                allow_fallback = false;
            }

            std::size_t data_size = data_.size();
            if (std::size_t(-1) == num_thread)
            {
                num_thread = curr_queue_++ % data_size;
            }
            else if (num_thread >= data_size)
            {
                num_thread %= data_size;
            }

            std::unique_lock<pu_mutex_type> l;
            num_thread = select_active_pu(l, num_thread, allow_fallback);

            HPX_ASSERT(thrd->get_scheduler_base() == this);
            HPX_ASSERT(num_thread < num_queues_);

            data_[num_thread].data_.queue_->schedule_thread(thrd, true);
        }

        /// Destroy the passed thread as it has been terminated
        void destroy_thread(
            threads::thread_data* thrd, std::int64_t& busy_count) override
        {
            HPX_ASSERT(thrd->get_scheduler_base() == this);
            thrd->get_queue<thread_queue_type>().destroy_thread(
                thrd, busy_count);
        }

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new items)
        std::int64_t get_queue_length(
            std::size_t num_thread = std::size_t(-1)) const override
        {
            // Return queue length of one specific queue.
            std::int64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < data_.size());

                return data_[num_thread].data_.queue_->get_queue_length();
            }

            for (std::size_t i = 0; i != data_.size(); ++i)
            {
                count += data_[i].data_.queue_->get_queue_length();
            }
            return count;
        }

        ///////////////////////////////////////////////////////////////////////
        // Queries the current thread count of the queues.
        std::int64_t get_thread_count(thread_state_enum state = unknown,
            thread_priority priority = thread_priority_default,
            std::size_t num_thread = std::size_t(-1),
            bool reset = false) const override
        {
            // Return thread count of one specific queue.
            std::int64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < data_.size());

                switch (priority)
                {
                case thread_priority_default:
                case thread_priority_low:
                case thread_priority_normal:
                case thread_priority_boost:
                case thread_priority_high:
                case thread_priority_high_recursive:
                    return data_[num_thread].data_.queue_->get_thread_count(
                        state);

                default:
                case thread_priority_unknown: {
                    HPX_THROW_EXCEPTION(bad_parameter,
                        "local_workstealing_scheduler::get_thread_count",
                        "unknown thread priority value "
                        "(thread_priority_unknown)");
                    return 0;
                }
                }
                return 0;
            }

            // Return the cumulative count for all queues.
            switch (priority)
            {
            case thread_priority_default:
            case thread_priority_low:
            case thread_priority_normal:
            case thread_priority_boost:
            case thread_priority_high:
            case thread_priority_high_recursive: {
                for (std::size_t i = 0; i != data_.size(); ++i)
                    count += data_[i].data_.queue_->get_thread_count(state);
                break;
            }

            default:
            case thread_priority_unknown: {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "local_workstealing_scheduler::get_thread_count",
                    "unknown thread priority value "
                    "(thread_priority_unknown)");
                return 0;
            }
            }
            return count;
        }

        ///////////////////////////////////////////////////////////////////////
        // Enumerate matching threads from all queues
        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const override
        {
            bool result = true;
            for (std::size_t i = 0; i != data_.size(); ++i)
            {
                result = result &&
                    data_[i].data_.queue_->enumerate_threads(f, state);
            }
            return result;
        }

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        ///////////////////////////////////////////////////////////////////////
        // Queries the current average thread wait time of the queues.
        std::int64_t get_average_thread_wait_time(
            std::size_t num_thread = std::size_t(-1)) const
        {
            // Return average thread wait time of one specific queue.
            std::uint64_t wait_time = 0;
            std::uint64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < data_.size());

                wait_time += data_[num_thread]
                                 .data_.queue_->get_average_thread_wait_time();
                return wait_time / (count + 1);
            }

            for (std::size_t i = 0; i != data_.size(); ++i)
            {
                wait_time +=
                    data_[i].data_.queue_->get_average_thread_wait_time();
                ++count;
            }

            return wait_time / (count + 1);
        }

        ///////////////////////////////////////////////////////////////////////
        // Queries the current average task wait time of the queues.
        std::int64_t get_average_task_wait_time(
            std::size_t num_thread = std::size_t(-1)) const
        {
            // Return average task wait time of one specific queue.
            std::uint64_t wait_time = 0;
            std::uint64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < data_.size());

                wait_time += data_[num_thread]
                                 .data_.queue_->get_average_task_wait_time();
                return wait_time / (count + 1);
            }

            for (std::size_t i = 0; i != data_.size(); ++i)
            {
                wait_time +=
                    data_[i].data_.queue_->get_average_task_wait_time();
                ++count;
            }

            return wait_time / (count + 1);
        }
#endif

        // return a random victim for the current stealing operation
        std::size_t random_victim(steal_request const& req) noexcept
        {
            std::size_t const data_size = data_.size();
            std::size_t bucket_size =
                (std::numeric_limits<std::int16_t>::max)() / data_size;

            // generate 3 random numbers max before resorting to more expensive
            // algorithm
            std::size_t result = 0;
            int attempts = 0;
            do
            {
                result = uniform_int_(gen_) / bucket_size;
                if (result < data_size && result != req.num_thread_ &&
                    !test(req.victims_, result))
                {
                    return result;
                }
            } while (++attempts < 3);

            // to avoid infinite trials we randomly select one of the possible
            // victims
            std::size_t const num_victims = data_size - count(req.victims_);

            // generate one more random number
            std::size_t selected_victim = 0;
            bucket_size =
                (std::numeric_limits<std::int16_t>::max)() / num_victims;
            do
            {
                selected_victim = uniform_int_(gen_) / bucket_size;
            } while (selected_victim >= num_victims);

            for (std::size_t i = 0; i != data_size; ++i)
            {
                if (!test(req.victims_, i))
                {
                    if (--selected_victim == 0)
                    {
                        result = i;
                        break;
                    }
                }
            }

            HPX_ASSERT(
                result >= 0 && result < data_size && result != req.num_thread_);

            return result;
        }

        // return the number of the next victim core
        std::size_t next_victim(
            scheduler_data& d, steal_request const& req) noexcept
        {
            std::size_t victim = std::size_t(-1);

            // return thief if max steal attempts has been reached
            if (req.attempt_ == data_.size() - 1)
            {
                // Return steal request to thief
                victim = req.num_thread_;
            }
            else
            {
                HPX_ASSERT(
                    (req.attempt_ == 0 && req.num_thread_ == d.num_thread_) ||
                    (req.attempt_ > 0 && req.num_thread_ != d.num_thread_));

#if defined(HPX_HAVE_WORKSTEALING_LAST_VICTIM)
                if (d.last_victim_ != std::uint16_t(-1))
                {
                    victim = d.last_victim_;
                }
                else
#endif
                {
                    victim = random_victim(req);
                }
            }

            // couldn't find victim, return steal request to thief
            if (victim == std::size_t(-1))
            {
                victim = req.num_thread_;
                HPX_ASSERT(victim != d.num_thread_);
            }

            HPX_ASSERT(victim < data_.size());
            HPX_ASSERT(0 <= req.attempt_ && req.attempt_ < data_.size());

            return victim;
        }

        // Every worker can have at most MAXSTEAL pending steal requests. A
        // steal request with idle == false indicates that the requesting worker
        // is still busy working on some tasks. A steal request with idle == true
        // indicates that the requesting worker is in fact idle and has nothing
        // to work on.
        void send_steal_request(scheduler_data& d, bool idle = true) noexcept
        {
            if (d.requested_ == 0)
            {
                steal_request req(
                    d.num_thread_, d.tasks_.get(), d.victims_, idle);
                std::size_t victim = next_victim(d, req);

                ++d.requested_;
                data_[victim].data_.requests_->set(std::move(req));

                ++d.steal_requests_sent_;
            }
        }

        // Try receiving tasks that are sent by another core as a response to
        // one of our steal requests.
        bool try_receiving_tasks(
            scheduler_data& d, std::size_t& added, thread_data** next_thrd)
        {
            task_data thrds;
            if (d.tasks_->get(&thrds))
            {
                --d.requested_;
                HPX_ASSERT(d.requested_ == 0);

                // if at least one thrd was received
                if (!thrds.tasks_.empty())
                {
                    // schedule all but the last thread
                    std::size_t received_threads = thrds.tasks_.size() - 1;
                    for (std::size_t i = 0; i != received_threads; ++i)
                    {
                        // schedule the received task to be picked up by the
                        // scheduler
                        HPX_ASSERT(thrds.tasks_[i] != nullptr);
                        d.queue_->schedule_thread(thrds.tasks_[i], true);
                        d.queue_->increment_num_stolen_to_pending();
                        ++added;
                    }

#if defined(HPX_HAVE_WORKSTEALING_LAST_VICTIM)
                    // store the originating core for the next stealing
                    // operation
                    d.last_victim_ = thrds.num_thread_;
                    HPX_ASSERT(d.last_victim_ != d.num_thread_);
#endif

                    if (next_thrd != nullptr)
                    {
                        // directly return the last thread as it should be run
                        // immediately
                        *next_thrd = thrds.tasks_.back();
                    }
                    else
                    {
                        d.queue_->schedule_thread(thrds.tasks_.back(), true);
                    }

                    d.queue_->increment_num_stolen_to_pending();
                    return true;
                }
            }
            return false;
        }

        // This is a function which gets called periodically by the thread
        // manager to allow for maintenance tasks to be executed in the
        // scheduler. Returns true if the OS thread calling this function
        // has to be terminated (i.e. no more work has to be done).
        bool wait_or_add_new(std::size_t num_thread, bool running,
            std::int64_t& idle_loop_count, bool enable_stealing,
            std::size_t& added, thread_data** next_thrd = nullptr) override
        {
            HPX_ASSERT(num_thread < data_.size());

            added = 0;

            auto& d = data_[num_thread].data_;
            bool result =
                d.queue_->wait_or_add_new(running, added, enable_stealing);

            // check if work was available
            if (0 != added)
                return result;

            // check if we have been disabled
            if (!running)
                return true;

            // return if no stealing is requested (or not possible)
            if (data_.size() == 1 || !enable_stealing)
                return result;

            // attempt to steal more work
            send_steal_request(d);
            HPX_ASSERT(d.requested_ != 0);

            // now try to handle steal requests again if we have not received a
            // task from some other core yet
            if (!try_receiving_tasks(d, added, next_thrd))
            {
                // decline or forward all pending steal requests
                decline_or_forward_all_steal_requests(d);
            }

#ifdef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
            // no new work is available, are we deadlocked?
            if (HPX_UNLIKELY(minimal_deadlock_detection && LHPX_ENABLED(error)))
            {
                bool suspended_only = true;

                for (std::size_t i = 0; suspended_only && i != data_.size();
                     ++i)
                {
                    suspended_only =
                        data_[i].data_.queue_->dump_suspended_threads(
                            i, idle_loop_count, running);
                }

                if (HPX_UNLIKELY(suspended_only))
                {
                    if (running)
                    {
                        LTM_(error)    //-V128
                            << "queue(" << num_thread << "): "
                            << "no new work available, are we "
                               "deadlocked?";
                    }
                    else
                    {
                        LHPX_CONSOLE_(
                            hpx::util::logging::level::error)    //-V128
                            << "  [TM] "                         //-V128
                            << "queue(" << num_thread << "): "
                            << "no new work available, are we "
                               "deadlocked?\n";
                    }
                }
            }
#endif
            return result;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) override
        {
            auto& d = data_[num_thread].data_;
            d.init(num_thread, data_.size(), this->thread_queue_init_);
            d.queue_->on_start_thread(num_thread);

            // create an empty mask that is properly sized
            resize(d.victims_, HPX_HAVE_MAX_CPU_COUNT);
            reset(d.victims_);
            set(d.victims_, num_thread);
        }

        void on_stop_thread(std::size_t num_thread) override
        {
            data_[num_thread].data_.queue_->on_stop_thread(num_thread);
        }

        void on_error(
            std::size_t num_thread, std::exception_ptr const& e) override
        {
            data_[num_thread].data_.queue_->on_error(num_thread, e);
        }

        void reset_thread_distribution() override
        {
            curr_queue_.store(0, std::memory_order_release);
        }

    protected:
        std::vector<util::cache_line_data<scheduler_data>> data_;

        std::atomic<std::size_t> curr_queue_;

        std::mt19937 gen_;
        std::uniform_int_distribution<std::int16_t> uniform_int_;

        detail::affinity_data const& affinity_data_;
    };
}}}       // namespace hpx::threads::policies

#include <hpx/config/warnings_suffix.hpp>

#endif
#endif
