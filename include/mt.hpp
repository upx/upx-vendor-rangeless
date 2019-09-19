/*
================================================================================
 
                             PUBLIC DOMAIN NOTICE
                 National Center for Biotechnology Information
 
  This software is a "United States Government Work" under the terms of the
  United States Copyright Act.  It was written as part of the author's official
  duties as a United States Government employees and thus cannot be copyrighted. 
  This software is freely available to the public for use. The National Library
  of Medicine and the U.S. Government have not placed any restriction on its use
  or reproduction.
 
  Although all reasonable efforts have been taken to ensure the accuracy and
  reliability of this software, the NLM and the U.S. Government do not and
  cannot warrant the performance or results that may be obtained by using this
  software. The NLM and the U.S. Government disclaim all warranties, expressed
  or implied, including warranties of performance, merchantability or fitness
  for any particular purpose.
 
  Please cite NCBI in any work or product based on this material.
 
================================================================================

  Author: Alex Astashyn

*/
#ifndef RANGELESS_MT_HPP_
#define RANGELESS_MT_HPP_

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <chrono>
#include <cassert>


/////////////////////////////////////////////////////////////////////////////

namespace rangeless { 
   
// Defining fn::end_seq here because that's all we need - 
// don't need to #include whole "fn.hpp".
//
// NB: definition shall be the same as in fn.hpp
#ifndef RANGELESS_FN_HPP_END_SEQ_EXCEPTION
#define RANGELESS_FN_HPP_END_SEQ_EXCEPTION
namespace fn
{
    struct end_seq
    {
        struct exception
        {};

        end_seq()
        {
            throw exception{};
        }

        template<typename T>
        operator T() const
        {
            throw exception{};
            return std::move(*static_cast<T*>(nullptr));
        }
    };
}
#endif


namespace mt { 

/////////////////////////////////////////////////////////////////////////////
/// \brief A simple timer.
struct timer
{
    /// returns the time elapsed since timer's instantiation, in seconds.
    /// To reset: `my_timer = mt::timer{}`.
    operator double() const
    {
        return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
            clock_t::now() - start_timepoint).count() * 1e-9;
    }
private:
    using clock_t = std::chrono::steady_clock;
    clock_t::time_point start_timepoint = clock_t::now();
};

/////////////////////////////////////////////////////////////////////////////
/// \brief Can be used as alternative to std::mutex.
///
/// It is faster than std::mutex, yet does not aggressively max-out the CPUs.
/// NB: may cause thread starvation
template<uint64_t SleepMicrosec = 20>
class atomic_lock
{    
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;

    // todo: could we implement exponential-backoff algorithm that prevents
    // thread-starvation in mpsc spmc scenarios?

public:

    atomic_lock() = default;

    // non-copyable and non-movable, as with std::mutex
    
               atomic_lock(const atomic_lock&) = delete;
    atomic_lock& operator=(const atomic_lock&) = delete;

               atomic_lock(atomic_lock&&) = delete;
    atomic_lock& operator=(atomic_lock&&) = delete;

    bool try_lock() noexcept
    {    
        return !m_flag.test_and_set(std::memory_order_acquire);
    }    

    void lock() noexcept
    {   
        static_assert(SleepMicrosec > 0, "");
        while(m_flag.test_and_set(std::memory_order_acquire)) { 
            
            std::this_thread::sleep_for(
                std::chrono::microseconds(SleepMicrosec));

            // Related:
            // https://stackoverflow.com/questions/7868235/why-is-sleeping-not-allowed-while-holding-a-spinlock
            //
            // Note that here we are sleepining while NOT holding 
            // the lock, but while trying to acquire it.
            // 
            // If we did NOT sleep here, the polling
            // thread would deadlock on a single-core system
            // if the thread holding the lock goes to sleep.
            //
            // Caveat emptor.
        }    
    }    

    void unlock() noexcept
    {    
        m_flag.clear(std::memory_order_release);
    }    
};   

class synchronized_queue_base
{
public:
    enum class status { success, closed, timeout };
    class queue_closed : public std::exception{};

    // NB: Core guideline T.62: Place non-dependent class template members in a non-templated base class
};


/////////////////////////////////////////////////////////////////////////////
/*! \brief Optionally-bounded blocking concurrent MPMC queue.
 *
 *   - Supports not-copy-constructible/not-default-constructible value-types (just requires move-assigneable).
 *   - Can be used with lockables other than `std::mutex`, e.g. `mt::atomic_lock`.
 *   - Contention-resistant: when used with `mt::atomic_lock` the throughput is comparable to state-of-the-art lock-free implementations.
 *   - Short and simple implementation using only c++11 standard library primitives.
 *   - Provides RAII-based closing semantics to communicate end-of-inputs 
 *     from the pushing end or failure/going-out-of-scope from the popping end.
 *
 * Related:
 * <br> <a href="http://www.boost.org/doc/libs/1_66_0/libs/fiber/doc/html/fiber/synchronization/channels/buffered_channel.html">`boost::fibers::buffered_channel`</a>
 * <br> <a href="http://www.boost.org/doc/libs/1_66_0/doc/html/thread/sds.html">`boost::sync_bounded_queue`</a>
 * <br> <a href="http://www.boost.org/doc/libs/1_66_0/doc/html/boost/lockfree/queue.html">`boost::lockfree::queue`</a>
 * <br> <a href="https://software.intel.com/en-us/node/506200">`tbb::concurrent_queue`</a>
 * <br> <a href="https://github.com/cameron314/concurrentqueue">`moodycamel::BlockingConcurrentQueue`</a>
 *
@code
    // A toy example to compute sum of lengths of strings in parallel.
    //
    // Spin-off a separate async-task that enqueues jobs 
    // to process a single input, and enqueues the 
    // futures into a synchronized queue, while accumulating 
    // the ready results from the queue in this thread.

    using queue_t = mt::synchronized_queue<std::future<size_t> >;
    queue_t queue{ 10 };

    auto fut = std::async(std::launch::async,[ &queue ]
    {
        auto close_on_exit = queue.close();

        for(std::string line; std::getline(std::cin, line); ) {
            queue <<= 
                std::async(
                    std::launch::async, 
                    [](const std::string& s) { 
                        return s.size(); 
                    },
                    std::move(line));
        }
    });

    size_t total = 0;
    queue >>= [&](queue_t::value_type x) { total += x.get(); };
    fut.get(); // rethrow exception, if any.
@endcode
*/
template <typename T, class BasicLockable = std::mutex /*or mt::atomic_lock*/>
class synchronized_queue : public synchronized_queue_base
{
public:
    using value_type = T;

    ///@{ 

    synchronized_queue(size_t capacity_ = size_t(-1))
        : m_capacity{ capacity_ == 0 ? 1 : capacity_ }
    {}

    /// NB: destructor closes the queue (non-blocking)
    ~synchronized_queue()
    {
        x_close();
    }

    // After careful consideration, decided not to provide 
    // move-semantics; copy and move constructors are implicitly
    // deleted.
    //
    // A synchronized_queue can be thought of buffered mutex
    // (i.e. a synchronization primitive rather than just a 
    // data structure), and mutexes are not movable.
    //
    // Problem: If a queue is moved while it is being actively used,
    // this will more than likely be unintentional and cause problems.
    //
    // Problem: In operator= we need to simultaneously lock
    // both mutexes; it will block if there's a blocked try_pop
    // call holding a lock on m_pop_mutex while waiting on 
    // m_can_swap.
    //
    // Problem: a closed queue may become reopened when moved-to.
    //    
    // Perhaps instead of moving the internals invasively,
    // move-assignment should be implemented by the other's
    // elements are inserted normally into `this`:
    //
    //    other.x_close();
    //    other >>= [this](value_type x) { *this <<= std::move(x); };
    //
    // Problems * Will throw if `this` is closed.
    //          * Will block if at capacity and there are no active poppers.
    //          * Modify capacity or keep original?
    //          * If the pipeline is inactive and non-empty,
    //            the user may expect the existing elements 
    //            to be erased rather than appended to.
    //
    // NB: boost::fiber::buffered_channel and boost::sync_bounded_queue
    // don't provide copy/move semantics either.

    ///@}

    ///@{

#if 0    
    /*
    Not sure if we need the inserter. Can instead use boost's iterator-adapter

    {{
        mt::synchronized_queue<long> queue;
        std::vector<long> vec{1,2,3,4,5};

        std::copy(vec.begin(), vec.end(),
                  boost::make_function_output_iterator(
                      [&queue](long x) { queue <<= x; }));
     }}
    */

    /////////////////////////////////////////////////////////////////////////
    struct insert_iterator //: public std::insert_iterator<synchronized_queue>
    {
        synchronized_queue& queue;

        insert_iterator& operator=(value_type val)
        {
            queue <<= std::move(val);
            return *this;
        }

        insert_iterator& operator*()     { return *this; }
        insert_iterator& operator++()    { return *this; }
        insert_iterator  operator++(int) { return *this; }
    };

    /// \brief `std::copy(it_begin, it_end, queue.inserter())`
    insert_iterator inserter()
    {
        return { *this };
    }
#endif

    /////////////////////////////////////////////////////////////////////////

    ///@}
    ///@{

    // NB: we're using operator<<= to represent 'enqueue' operation 
    // in synchronized_queue<...>, synchronized<...>, pool<...>, and pipeline<...>.
    //
    // boost/thread/concurrent_queues use operator<< and operator>>,
    // but when reading the code that uses '<<', especially when it 
    // involves dealing with iostreams, it may get a bit confusing
    // as to what's going on.
    //
    // Additionally, as << is heavily overloaded, type-error generate
    // insanely-long compiler errors, so I'd rather avoid using 
    // operators at all, than touch '<<'.
    //
    // '<<=' is right-associative so it's of no use returning 
    // *this from it. However right-associativity in our context
    // makes more sense, e.g. queue <<= pool <<= []{ ... };

    /// \brief Blocking push. May throw `queue_closed`
    void operator<<=(value_type val) noexcept(false)
    {
        // NB: if this throws, val is gone. If the user needs 
        // to preserve it in case of failure (strong exception
        // guarantee), it should be using try_push which takes 
        // value by rvalue-reference and moves it only in case of success.
        //
        // We could do the same here, but that would mean
        // that the user must always pass as rvalue, and 
        // to pass by copy it would have to do so explicitly, e.g.
        //
        // void operator<<=(value_type&& val);
        // queue <<= std::move(my_value);
        // queue <<= my_const_ref; // won't compile.
        // queue <<= queueue_t::value_type( my_const_ref ); // explicit copy
        //
        // I think this would actually be a good thing, as it 
        // makes the copying visible, but all other synchronied-queue
        // APIs allow pushing by const-or-forwarding-references, 
        // so we have allow the same for the sake of consistency.
        //
        // (could also accomplish the same by taking as forwarding
        // reference: template<typename U> operator<<=(U&& val);

        const status st = try_push(std::move(val), s_max_timeout());

        assert(st != status::timeout);

        if(st == status::closed) {
            throw queue_closed{};
        }

        assert(st == status::success);
    }

    void push(value_type val) noexcept(false)
    {
        this->operator<<=(std::move(val));
    }

    /////////////////////////////////////////////////////////////////////////
    /// Blocking pop. May throw `queue_closed`
    value_type pop() noexcept(false)
    {
        guard_t g{ m_pop_mutex };

        if(!m_pop_queue.empty()) {
            return x_pop();
        }

        const status st = x_swap_queues(s_max_timeout());

        assert(st != status::timeout);
        
        if(st == status::closed) {
            throw queue_closed{};
        } 
        
        assert(st == status::success);
        return x_pop();
    }

#if 0 // this is redundant - just use x = queue.pop();

    /// Blocking pop into an lvalue-reference. May throw `queue_closed`.
    void operator>>=(value_type& val) noexcept(false)
    {
        val = this->pop();
    }
#endif

    /// \brief pop() the values into the provided sink-function until closed and empty.
    ///
    /// e.g. `queue >>= [&out_it](T x){ *out_it++ = std::move(x); };`
    /// <br>Queue is automatically closed if exiting via exception, unblocking the pushers.
    template<typename F>
    auto operator>>=(F&& sink) -> decltype((void)sink(this->pop()))
    {
        auto guard = this->close();

        while(true) {
            bool threw_in_pop = true;

            try {
                value_type val = this->pop();
                threw_in_pop = false;
                sink(std::move(val));

            } catch(queue_closed&) {

                if(threw_in_pop) {
                    break; // threw in pop() - ours
                } else {
                    throw; // threw in sink() - not ours - rethrow;
                    //
                    // This could be an unhandled exception from
                    // sink that is from some different queue that we
                    // shouldn't be intercepting.
                    //
                    // If sink intends to close the queue
                    // (e.g. break-out), it can do it explicitly
                    // via the close-guard;
                }
            }
        }
        
        assert(closed() && empty());
    }

#if 0
    // implemented as overload of pipeline::invoke(...) instead
    
    /// \brief pop() the values into the pipeline until close and empty, then flush the pipeline.
    template<typename Pipeline>
    auto operator>>=(Pipeline&& pipeline)
      -> decltype((void)(pipeline <<= this->pop()))
    {
        this->operator>>=(
                [&](value_type x){ 
                    pipeline <<= std::move(x);
                });
        impl::flush_if_can(pipeline);
    }
#endif

    ///@}

    ///@{ 

    /////////////////////////////////////////////////////////////////////////
    /// In case of success, the value will be moved-from.
    template <typename Duration = std::chrono::milliseconds>
    status try_push(value_type&& value, Duration timeout = {})
    {
        // NB: we could have taken value as value_type&, but
        // the user-code may forget or not expect that the value 
        // will be moved-from and may try to use it.
        //
        // With rvalue-reference the caller-code has to, e.g.
        //     auto state = queue.try_push(std::move(x));
        //
        // making the move explicitly visible.

        lock_t lock{ m_push_mutex };
        
        const bool ok = m_can_push.wait_for( 
                lock,
                timeout,
                [this]{ return m_size < m_capacity
                            || m_push_queue.empty()
                            || m_closed; 
                        // NB: m_size is non-increasing here.
                        // We're also allowing push if empty,
                        // in case (m_size<m_capacity) evaluated to `false`
                        // above and just became `true` before we return,
                        // causing us to block until cv is notified again.
                        // We want to push-queue to not be emty so that
                        // x_swap_queues() will not block.
                      });

        if(!ok) {
            return status::timeout;
        }

        if(m_closed) {
             return status::closed;
        }

        assert(m_size < m_capacity);


        // if push throws, is the value moved-from?
        // No. std::move is just an rvalue_cast - no-op
        // if the move-assignment never happens.
        m_push_queue.push(std::move(value));

        ++m_size;
        m_can_swap.notify_one();

        // This guard is not formally necessary, but it's a preformance
        // optimization for heavy-contention case, e.g. 128 pushers
        // and 128 poppers on a 32-core host. There's a single thread
        // waiting on m_can_swap trying to get a lock vs. 128 pushers, 
        // resulting in starvation of the thread trying to x_swap_queues().
        //
        // When we call unlock below, it will unblock another try_push
        // that will do its thing up to this point, but it will not be
        // able to call unlock() until this thread aux_guard releases.
        // This throttles the pushing threads, allowing the swapping 
        // thread to grab the lock.
        /*
            pushers throughput
            /popper (M/s)
                
        without this guard:
            1/1     4.869   *************************************************
            1/32    1.437   ***************
            32/1    0.7439  ********
            16/16   0.8658  *********
            128/128 0.3944  ****

        with this guard:
            1/1     2.292   ***********************
            1/32    1.469   ***************
            32/1    1.337   **************
            16/16   1.176   ************
            128/128 1.816   *******************
        */
        guard_t aux_guard{ m_push_mutex_aux };

        lock.unlock();
        return status::success;
    }

    /////////////////////////////////////////////////////////////////////////
    /// In case of success, the value will be move-assigned.
    template <typename Duration = std::chrono::milliseconds>
    status try_pop(value_type& value, Duration timeout = {})
    {
        guard_t g{ m_pop_mutex };

        if(!m_pop_queue.empty()) {
            value = x_pop();
            return status::success;
        }

        const status st = x_swap_queues(timeout);
        
        if(st == status::success) {
            value = x_pop();
        }

        return st;
    }

    ///@}
    
    ///@{ 

    /////////////////////////////////////////////////////////////////////////
    size_t size() const noexcept
    {
        return m_size;
    }

    bool empty() const noexcept
    {
        return m_size == 0;
    }

    size_t capacity() const noexcept
    {
        return m_capacity;
    }

    bool closed() const noexcept
    {
        return m_closed;
    }

    /////////////////////////////////////////////////////////////////////////
    struct close_guard
    {
    private:
        synchronized_queue* ptr;

    public:
        close_guard(synchronized_queue& queue) : ptr{ &queue }
        {}

        void reset()
        {
            ptr = nullptr;
        }

        ~close_guard()
        {
            if(ptr) {
                ptr->x_close();
            }
        }
    };

    /// \brief Return an RAII object that will close the queue in its destructor.
    /// 
    /// @code
    /// auto guard = queue.close(); // close the queue when leaving scope
    /// queue.close(); // close the queue now (guard's is destroyed immediately)
    /// @endcode
    ///
    /// <br> NB: closing is non-blocking.
    /// <br>Blocked calls to try_push()/try_pop() shall return with status::closed.
    /// <br>Blocked calls to push()/pop() shall throw `queue_closed`.
    /// <br>Subsequent calls to push()/try_push() shall do as above.
    /// <br>Subsequent calls to pop()/try_pop() will succeed
    ///   until the queue becomes empty, and throw/return-closed thereafter.
    close_guard close() noexcept
    {
       return close_guard{ *this };
    }

    ///@}

private:
    using guard_t = std::lock_guard<BasicLockable>;
    using  lock_t = std::unique_lock<BasicLockable>;

    // NB: can't use chrono::seconds::max() for timeout, because
    // now() + max() within wait_for will overflow.
    static constexpr std::chrono::hours s_max_timeout() 
    {
        return std::chrono::hours(24*365*200); 
        // 200 years ought to be enough for everyone
    }

    // precondition: m_pop_queue is not empty and m_pop_mutex is locked
    value_type x_pop()
    {
        assert(!m_pop_queue.empty());
        assert(m_size > 0);
        value_type val = std::move(m_pop_queue.front());
        m_pop_queue.pop();
        --m_size;
        m_can_push.notify_one();
        return val;
    }
    
    /////////////////////////////////////////////////////////////////////////
    // precondition: m_pop_queue is empty and m_pop_mutex is locked
    template<typename Duration>
    status x_swap_queues(Duration timeout)
    {
        assert(m_pop_queue.empty());

        m_can_push.notify_one(); // in case the pushing thread
                                 // is in false-wait (see corresponding wait_for)

        lock_t push_lock{ m_push_mutex };

        // NB: m_pop_mutex is locked while we're waiting
        // on the (notempty or closed) condition, but 
        // this is the only place it is used.

        const bool ok = m_can_swap.wait_for(
                push_lock, 
                timeout,
                [this]{ return !m_push_queue.empty()
                             || m_closed; 
                      });

        if(!ok) {
            return status::timeout;
        }

        if(m_push_queue.empty() && m_closed) {
            return status::closed;
        }
        // !(empty && closed) && (!empty || closed)
        // ^^if-statement        ^^wait-for condition
        //
        // simplifies to: !empty

        assert(!m_push_queue.empty());
        assert( m_pop_queue.empty());

        std::swap(m_push_queue, m_pop_queue); // this is O(1)
        return status::success;
    }

    /////////////////////////////////////////////////////////////////////////
    /// \brief Closes the queue for more pushing.
    ///
    void x_close()
    {
        // m_can_push and m_can_swap wait-conditions may evaluate to false,
        // and then m_closed becomes true before the lambda returns the 
        // original (now incorrect) value, so the wait_for will deadlock because
        // notify_all() above happened before the thread is about to block.
        //
        // Therefore we need to lock the associated mutexes first.
        // If we try to lock m_pop_mutex here, we may deadlock because
        // it may be locked while waiting on m_can_swap. Therefore we need
        // to unblock wait_for on m_push_mutex in x_swap_queue().
        {{
             guard_t g{ m_push_mutex }; 
             m_closed = true;
        }}
        m_can_swap.notify_all();
        m_can_push.notify_all();
    }

    // NB: open() is not provided, such that if closed() returns true,
    // we know for sure that it's staying that way. 


    /////////////////////////////////////////////////////////////////////////

    // To reduce lock contention between producers and consumers
    // we're using separate queues for pushing and popping,
    // and lock them both and swap as necessary.

    using queue_t = std::queue<value_type>;

    using condvar_t = typename std::conditional<
        std::is_same<BasicLockable, std::mutex>::value,
            std::condition_variable,
            std::condition_variable_any        >::type;

                 size_t m_capacity; // protected by push_mutex
     std::atomic_size_t m_size = { 0 };
       std::atomic_bool m_closed = { false };

             const char padding1[128] = {}; // avoid false-sharing

                queue_t m_push_queue;
          BasicLockable m_push_mutex;
          BasicLockable m_push_mutex_aux;
              condvar_t m_can_swap;

             const char padding2[128] = {};

                queue_t m_pop_queue;
          BasicLockable m_pop_mutex;
              condvar_t m_can_push;   

             const char padding3[128] = {};

    // Notes: 
    //
    // Could use a single ring-buffer instead of pair of queues, and two
    // cursors, m_head and m_tail. Using the pair-of-queues allows
    // the size to grow dynamically, without preallocating the storage.
    //
    // Throughput for typical usage is limited by mt-performance of malloc,
    // rather than synchronized_queue, when the memory is allocated in
    // one thread and freed in another (e.g. future/promise shared-state, 
    // allocating memory for results in a worker-thread and passing the 
    // ownership to consumer thread, etc. In this case malloc has to 
    // synchronize memory ownership among threads under the hood. 
    //
    // MT-optimized allocators, such as libtcmalloc or libllalloc are 
    // much faster in this scenario.
    //
    // NB: Using fancy wait-free queue in parallelized-pipeline implementation,
    // or substituting mutexes with atomic_locks in this implementation
    // actually hurts performance somewhat in cpu-bound applications
    // even though it increases queue throughput for POD types.

}; // synchronized_queue


} // namespace mt
} // namespace rangeless

#endif // RANGELESS_MT_HPP_ 
