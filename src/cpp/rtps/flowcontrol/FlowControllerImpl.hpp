#ifndef _RTPS_FLOWCONTROL_FLOWCONTROLLERIMPL_HPP_
#define _RTPS_FLOWCONTROL_FLOWCONTROLLERIMPL_HPP_

#include <fastdds/rtps/flowcontrol/FlowController.hpp>
#include <fastdds/rtps/common/Guid.h>
#include <fastdds/rtps/writer/RTPSWriter.h>

#include <map>
#include <thread>
#include <mutex>
#include <cassert>
#include <condition_variable>

namespace eprosima {
namespace fastdds {
namespace rtps {

/** Classes used to specify FlowController's publication model **/

//! Only sends new samples synchronously. There is no mechanism to send old ones.
struct FlowControllerPureSyncPublishMode {};

//! Sends new samples asynchronously. Old samples are sent also asynchronously */
struct FlowControllerAsyncPublishMode
{
    FlowControllerAsyncPublishMode()
    {
        headinterested.writer_info.next = &tailinterested;
        tailinterested.writer_info.previous = &headinterested;
        head.writer_info.next = &tail;
        tail.writer_info.previous = &head;
    }

    virtual ~FlowControllerAsyncPublishMode()
    {
        if (running)
        {
            std::unique_lock<std::mutex> lock(changes_interested_mutex);
            assert(&tailinterested == headinterested.writer_info.next);
            running = false;
            cv.notify_one();
            changes_interested_mutex.unlock();
            thread.join();
        }
    }

    std::thread thread;

    bool running = false;

    std::condition_variable cv;

    //! Mutex for interested samples to be added.
    // TODO Improve this mutex with simple atomic? spin mutex.
    std::mutex changes_interested_mutex;

    //! Head element of interested changes list to be included.
    //! Should be protected with changes_interested_mutex.
    fastrtps::rtps::CacheChange_t headinterested;

    //! Tail element of interested changes list to be included.
    //! Should be protected with changes_interested_mutex.
    fastrtps::rtps::CacheChange_t tailinterested;

    //! Head element on the queue.
    //! Should be protected with mutex_.
    fastrtps::rtps::CacheChange_t head;

    //! Tail element on the queue.
    //! Should be protected with mutex_.
    fastrtps::rtps::CacheChange_t tail;

    //! Used to warning async thread a writer wants to remove a sample.
    std::atomic<uint32_t> writers_interested_in_remove = {0};
};

//! Sends new samples synchronously. Old samples are sent asynchronously */
struct FlowControllerSyncPublishMode : public FlowControllerPureSyncPublishMode, FlowControllerAsyncPublishMode {};


/** Classes used to specify FlowController's sample scheduling **/

//! Fifo scheduling
struct FlowControllerFifoSchedule {};

//! Round Robin scheduling
struct FlowControllerRoundRobinSchedule {};

//! High priority scheduling
struct FlowControllerHighPrioritySchedule {};

//! Priority with reservation scheduling
struct FlowControllerPriorityWithReservation {};

template<typename PublishMode, typename SampleScheduling>
class FlowControllerImpl : public FlowController
{
    using publish_mode = PublishMode;

public:

    virtual ~FlowControllerImpl() = default;

    /*!
     * Initializes the flow controller.
     */
    void init() override
    {
        initialize_async_thread();
    }

    /*!
     * Registers a writer.
     * This object is only be able to manage a CacheChante_t if its writer was registered previously with this function.
     *
     * @param writer Pointer to the writer to be registered. Cannot be nullptr.
     */
    void register_writer(
            fastrtps::rtps::RTPSWriter* writer) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        writers_.insert({ writer->getGuid(), writer});
    }

    /*!
     * Unregister a writer.
     *
     * @param writer Pointer to the writer to be unregistered. Cannot be nullptr.
     */
    void unregister_writer(
            fastrtps::rtps::RTPSWriter* writer) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        writers_.erase(writer->getGuid());
    }

    /*
     * Adds the CacheChange_t to be managed by this object.
     * The CacheChange_t has to be a new one, that is, it has to be added to the writer's history before this call.
     * This function should be called by RTPSWriter::unsent_change_added_to_history().
     * This function has two specializations depending on template parameter PublishMode.
     *
     * @param Pointer to the writer which the added CacheChante_t is responsable. Cannot be nullptr.
     * @param change Pointer to the new CacheChange_t to be managed by this object. Cannot be nullptr.
     * @return true if sample could be added. false in other case.
     */
    bool add_new_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time) override
    {
        return add_new_sample_impl(writer, change, max_blocking_time);
    }

    /*!
     * Adds the CacheChante_t to be managed by this object.
     * The CacheChange_t has to be an old one, that is, it is already in the writer's history and for some reason has to
     * be sent again.
     *
     * @param Pointer to the writer which the added change is responsable. Cannot be nullptr.
     * @param change Pointer to the old change to be managed by this object. Cannot be nullptr.
     * @return true if sample could be added. false in other case.
     */
    bool add_old_sample(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change) override
    {
        return add_old_sample_impl(writer, change,
                       std::chrono::steady_clock::now() + std::chrono::hours(24));
    }

    /*!
     * If currently the CacheChange_t is managed by this object, remove it.
     * This funcion should be called when a CacheChange_t is removed from the writer's history.
     *
     * @param Pointer to the change which should be removed if it is currently managed by this object.
     */
    void remove_change(
            fastrtps::rtps::CacheChange_t* change) override
    {
        assert(nullptr != change);
        remove_change_impl(change);
    }

private:

    /*!
     * Initialize asynchronous thread.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    initialize_async_thread()
    {
        if (false == async_mode.running)
        {
            // Code for initializing the asynchronous thread.
            async_mode.running = true;
            async_mode.thread = std::thread(&FlowControllerImpl::run, this);
        }
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case the async thread doesn't need to be initialized.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    initialize_async_thread()
    {
        // Do nothing.
    }

    /*!
     * This function tries to send the sample synchronously.
     * That is, it uses the user's thread, which is the one calling this function, to send the sample.
     * It calls new function `RTPSWriter::deliver_sample()` for sending the sample.
     * If this function fails (for example because non-blocking socket is full), this function stores internally the sample to
     * try sending it again asynchronously.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_base_of<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_new_sample_impl(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time)
    {
        // This call should be made with writer's mutex locked.
        if (!writer->deliver_sample_nts(change, max_blocking_time))
        {
            // Sync delivery failes. Try to store for asynchronous delivery.
            return add_old_sample_impl(writer, change, max_blocking_time);
        }
    }

    /*!
     * This function stores internally the sample to send it asynchronously.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_base_of<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_new_sample_impl(
            fastrtps::rtps::RTPSWriter* writer,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& max_blocking_time)
    {
        return add_old_sample_impl(writer, change, max_blocking_time);
    }

    /*!
     * This function store internally the sample and wake up the async thread.
     *
     * @note Before calling this function, the change's writer mutex have to be locked.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_old_sample_impl(
            fastrtps::rtps::RTPSWriter*,
            fastrtps::rtps::CacheChange_t* change,
            const std::chrono::time_point<std::chrono::steady_clock>& /* TODO max_blocking_time*/)
    {
        // This comparison is thread-safe, because we ensure the change to a problematic state is always protected for
        // its writer's mutex.
        // Problematic states:
        // - Being added: change both pointers from nullptr to a pointer values.
        // - Being removed: change both pointer from pointer values to nullptr.
        if (nullptr == change->writer_info.previous &&
                nullptr == change->writer_info.next)
        {
            std::unique_lock<std::mutex> lock(async_mode.changes_interested_mutex);
            change->writer_info.previous = async_mode.tailinterested.writer_info.previous;
            change->writer_info.previous->writer_info.next = change;
            async_mode.tailinterested.writer_info.previous = change;
            change->writer_info.next = &async_mode.tailinterested;

            async_mode.cv.notify_one();

            return true;
        }

        return false;
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case there is no async mechanism.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, bool>::type
    add_old_sample_impl(
            fastrtps::rtps::RTPSWriter*,
            fastrtps::rtps::CacheChange_t*,
            const std::chrono::time_point<std::chrono::steady_clock>&)
    {
        // Do nothing. Fail.
        return false;
    }

    /*!
     * This function store internally the sample and wake up the async thread.
     *
     * @note Before calling this function, the change's writer mutex have to be locked.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<!std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    remove_change_impl(
            fastrtps::rtps::CacheChange_t* change)
    {
        // This comparison is thread-safe, because we ensure the change to a problematic state is always protected for
        // its writer's mutex.
        // Problematic states:
        // - Being added: change both pointers from nullptr to a pointer values.
        // - Being removed: change both pointer from pointer values to nullptr.
        if (nullptr != change->writer_info.previous ||
                nullptr != change->writer_info.next)
        {
            ++async_mode.writers_interested_in_remove;
            std::unique_lock<std::mutex> lock(mutex_);
            std::unique_lock<std::mutex> interested_lock(async_mode.changes_interested_mutex);

            // When blocked, both pointer are different than nullptr or equal.
            assert((nullptr != change->writer_info.previous &&
                    nullptr != change->writer_info.next) ||
                    (nullptr == change->writer_info.previous &&
                    nullptr == change->writer_info.next));

            // Try to join previous node and next node.
            change->writer_info.previous->writer_info.next = change->writer_info.next;
            change->writer_info.next->writer_info.previous = change->writer_info.previous;
            change->writer_info.previous = nullptr;
            change->writer_info.next = nullptr;
            --async_mode.writers_interested_in_remove;
        }
    }

    /*! This function is used when PublishMode = FlowControllerPureSyncPublishMode.
     *  In this case there is no async mechanism.
     */
    template<typename PubMode = PublishMode>
    typename std::enable_if<std::is_same<FlowControllerPureSyncPublishMode, PubMode>::value, void>::type
    remove_change_impl(
            fastrtps::rtps::CacheChange_t*)
    {
        // Do nothing. Fail.
    }

    /*!
     * Function ran by asynchronous thread.
     */
    void run()
    {
        while (async_mode.running)
        {
            // There is writers interested in remove a sample.
            if (0 != async_mode.writers_interested_in_remove)
            {
                continue;
            }

            std::unique_lock<std::mutex> lock(mutex_);

            //Check if we have to sleep.
            {
                bool queue_empty =  &async_mode.tail == async_mode.head.writer_info.next;
                std::unique_lock<std::mutex> in_lock(async_mode.changes_interested_mutex);
                // Add interested changes into the queue.
                bool new_ones = add_interested_changes_to_queue_nts();

                while (queue_empty && !new_ones && async_mode.running)
                {
                    lock.unlock();
                    async_mode.cv.wait(in_lock);

                    in_lock.unlock();
                    lock.lock();
                    in_lock.lock();

                    queue_empty =  &async_mode.tail == async_mode.head.writer_info.next;
                    new_ones = add_interested_changes_to_queue_nts();
                }
            }

            while (&async_mode.tail != async_mode.head.writer_info.next)
            {
                fastrtps::rtps::CacheChange_t* change_to_process = async_mode.head.writer_info.next;
                auto writer_it = writers_.find(change_to_process->writerGUID);
                assert(writers_.end() != writer_it);

                if (!try_lock(writer_it->second))
                {
                    // Unlock mutex_ and try again.
                    break;
                }

                if (!writer_it->second->deliver_sample_nts(change_to_process,
                        std::chrono::steady_clock::now() + std::chrono::hours(24)))
                {
                    unlock(writer_it->second);
                    // Unlock mutex_ and try again.
                    break;
                }

                // Remove from queue.
                change_to_process->writer_info.previous->writer_info.next = change_to_process->writer_info.next;
                change_to_process->writer_info.next->writer_info.previous = change_to_process->writer_info.previous;
                change_to_process->writer_info.previous = nullptr;
                change_to_process->writer_info.next = nullptr;


                unlock(writer_it->second);

                if (0 != async_mode.writers_interested_in_remove)
                {
                    // There are writers that want to remove samples.
                    break;
                }

                // Add interested changes into the queue.
                std::unique_lock<std::mutex> lock(async_mode.changes_interested_mutex);
                add_interested_changes_to_queue_nts();
            }
        }
    }

    /*!
     * Returns the first sample in the queue.
     * Default behaviour.
     * Expects the queue is ordered.
     *
     * @return Pointer to next change to be sent. nullptr implies there is no sample to be sent or is forbidden due to
     * bandwidth exceeded.
     */
    fastrtps::rtps::CacheChange_t* get_next_change_nts()
    {
        /*
           // Check if there is available bandwidth to send a samples.
           fastrtps::rtps::CacheChange_t* change = first_;
           first_ = change->writer_info.next;
           change->writer_info.next = nullptr;
           return first_;
         */
    }

    /*!
     * Store the sample at the end of the list
     * when SampleScheduling == FlowControllerFifoSchedule.
     *
     * @return true if there is added changes.
     */
    template<typename Scheculing = SampleScheduling>
    typename std::enable_if<std::is_same<FlowControllerFifoSchedule, Scheculing>::value, bool>::type
    add_interested_changes_to_queue_nts()
    {
        // This function should be called with mutex_  and interested_lock locked, because the queue is changed.
        bool returned_value = false;

        // TODO Remember with best effor this is not work if is transient local adding 1,2,3 and 3 is in the queue.
        fastrtps::rtps::CacheChange_t* interested_it = async_mode.headinterested.writer_info.next;
        fastrtps::rtps::CacheChange_t* next_it = nullptr;
        while (&async_mode.tailinterested != interested_it)
        {
            next_it = interested_it->writer_info.next;
            interested_it->writer_info.previous->writer_info.next = interested_it->writer_info.next;
            interested_it->writer_info.next->writer_info.previous = interested_it->writer_info.previous;
            interested_it->writer_info.previous = async_mode.tail.writer_info.previous;
            interested_it->writer_info.previous->writer_info.next = interested_it;
            async_mode.tail.writer_info.previous = interested_it;
            interested_it->writer_info.next = &async_mode.tail;

            interested_it = next_it;
            returned_value = true;
        }

        return returned_value;
    }

    std::mutex mutex_;

    std::map<fastrtps::rtps::GUID_t, fastrtps::rtps::RTPSWriter*> writers_;

    publish_mode async_mode;
};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // _RTPS_FLOWCONTROL_FLOWCONTROLLERIMPL_HPP_
