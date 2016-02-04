#pragma once

#include <stxxl/bits/common/utils.h>

#include <thread>
#include <mutex>
#include <condition_variable>

#include <vector>

template <typename StreamIn, typename T = typename StreamIn::value_type>
class AsyncStream {
    using BufferType = std::vector<T>;
    constexpr static unsigned int buffer_size = (1<<20) / sizeof(T);

    std::vector<BufferType> _buffers;
    std::condition_variable _buffer_cv;
    std::mutex _buffer_mutex;

    // producer port
    StreamIn &  _producing_stream;
    uint32_t _producing_buffer_index;
    bool _producing_done;
    // consumer port
    uint32_t _consume_buffer_index;

    typename BufferType::const_iterator _consume_read_iterator;
    typename BufferType::const_iterator _consume_end_iterator;
    bool _consume_acquired;
    bool _consume_empty;
    bool _consume_done;

    std::thread _producing_thread;

    unsigned int _next_buffer_index(unsigned int i) const {
        return (i+1) % _buffers.size();
    }

    void _producer_copy_to_buffers() {
        _buffers.resize(buffer_size);

        while(1) {
            // fill buffer
            auto & buffer = _buffers[_producing_buffer_index];
            buffer.reserve(buffer_size);
            for(unsigned int i=0; i<buffer_size && !_producing_stream.empty(); ++i, ++_producing_stream) {
                buffer.push_back(*_producing_stream);
            }

            // advance to next index (indicate, that the recently filled buffer
            // is available for consumption) but not before our new buffer is
            // available
            {
                std::unique_lock<std::mutex> lock(_buffer_mutex);
                auto next_idx = _next_buffer_index(_producing_buffer_index);
                _buffer_cv.wait(lock, [&]() {return _consume_buffer_index != next_idx || _consume_done;});
                _producing_buffer_index = next_idx;

                if (_consume_done)
                    break;

                if (_producing_stream.empty()) {
                    _producing_done = true;
                    _buffer_cv.notify_one();
                    break;
                }

                _buffer_cv.notify_one();
            }
        }
    }

    static void _producer_main(AsyncStream* o) {
        o->_producer_copy_to_buffers();
    }

    void _consume_acquire_buffer() {
        std::unique_lock<std::mutex> lock(_buffer_mutex);

        if (_consume_acquired) {
            // since we are currently holding a buffer, let's give it back
            // by advancing the consuming index
            _consume_buffer_index = _next_buffer_index(_consume_buffer_index);
        }

        _buffer_cv.wait(lock, [&]() {
            return (_consume_buffer_index != _producing_buffer_index) || _producing_done;
        });

        _consume_empty = (_consume_buffer_index == _producing_buffer_index);
        _consume_acquired = true;

        _consume_read_iterator = _buffers[_consume_buffer_index].begin();
        _consume_end_iterator  = _buffers[_consume_buffer_index].end();

        _buffer_cv.notify_one();
    }




public:
    /**
     * @param stream        The stream producing data
     * @param auto_acquire  Calls acquire() in constructor and hence offers
     *                      a STXXL conform streaming interface. Slow!
     * @param number_of_buffers  Must be >2
     */
    AsyncStream(StreamIn & stream, bool auto_acquire = true, unsigned int number_of_buffers = 3)
        : _buffers(number_of_buffers)

        , _producing_stream(stream)
        , _producing_buffer_index(0)
        , _producing_done(false)

        , _consume_buffer_index(0)
        , _consume_acquired(false)
        , _consume_empty(false)
        , _consume_done(false)

        , _producing_thread(_producer_main, this)
    {
        assert(number_of_buffers > 2);
        if (auto_acquire) acquire();
    }

    ~AsyncStream() {
        if (_producing_thread.joinable()) {
            // producer is still running, so stop it
            {
                std::unique_lock<std::mutex> lock(_buffer_mutex);
                _consume_done = true;
            }

            _buffer_cv.notify_one();
            _producing_thread.join();
        }
    }

    /**
     * Call this function BEFORE the first access to the public streaming interface.
     * Since this operator is blocking, you want to do it as late as possible in order
     * to give the producer enough time to offer a buffer.
     */
    void acquire() {
        assert(!_consume_acquired);
        _consume_acquire_buffer();
    }

    bool empty() const {
        assert(_consume_acquired);
        return _consume_empty;
    }

    const T&operator*() const {
        assert(_consume_acquired);
        return *_consume_read_iterator;
    }

    AsyncStream& operator++() {
        if (UNLIKELY(!_consume_acquired))
            _consume_acquire_buffer();

        if (UNLIKELY(++_consume_read_iterator == _consume_end_iterator)) {
            _consume_acquire_buffer();
        }

        return *this;
    }
};
