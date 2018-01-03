#pragma once

#include "asio.hpp"

#include <atomic>

#include <assert.h>

// set SO_NODELAY and enable keepalive
void configureSocket(asio::ip::tcp::socket& socket);

bool resetCurrentDirectory();

void trace(const char* msg);

constexpr size_t buffered_handler_storage_size = 256;

// allocates using internal buffer, with fallback to the heap if full
class BufferedHandlerAllocator
{
public:
	BufferedHandlerAllocator()
	{}

	BufferedHandlerAllocator(const BufferedHandlerAllocator&) = delete;
	BufferedHandlerAllocator& operator=(const BufferedHandlerAllocator&) = delete;

	void* allocate(std::size_t size)
	{
		// in debug mode, warn if we actually have to allocate from heap
#ifdef _DEBUG
		if (size > buffered_handler_storage_size) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				assert(false);
			}
		}
#endif
		if (size <= buffered_handler_storage_size)
		{
			// if test_and_set returns 0, then it was not previously in use, and we can use it!
			if (!in_use_.test_and_set()) {
				return &storage_;
			}
		}
		
		return ::operator new(size);
	}

	void deallocate(void* pointer)
	{
		if (pointer == &storage_)
		{
			in_use_.clear();
		}
		else
		{
			::operator delete(pointer);
		}
	}

private:
	// Storage space used for handler-based custom memory allocation.
	typename std::aligned_storage<buffered_handler_storage_size>::type storage_;

	// Whether the handler-based custom allocation storage has been used.
	std::atomic_flag in_use_ = ATOMIC_FLAG_INIT;
};

// handler to override asio_handler_allocate and asio_handler_deallocate
// so it re-uses a BufferedHandlerAllocator to store the handler
template <typename Handler>
class BufferedHandler
{
public:
	BufferedHandler(BufferedHandlerAllocator& a, Handler h)
		: allocator_(a),
		handler_(h)
	{
	}

	template <typename ...Args>
	void operator()(Args&&... args)
	{
		handler_(std::forward<Args>(args)...);
	}

	friend void* asio_handler_allocate(std::size_t size,
		BufferedHandler<Handler>* this_handler)
	{
		return this_handler->allocator_.allocate(size);
	}

	friend void asio_handler_deallocate(void* pointer, std::size_t /*size*/,
		BufferedHandler<Handler>* this_handler)
	{
		this_handler->allocator_.deallocate(pointer);
	}

private:
	BufferedHandlerAllocator& allocator_;
	Handler handler_;
};

template <typename Handler>
inline BufferedHandler<Handler> MakeBufferedHandler(
	BufferedHandlerAllocator& a, Handler h)
{
	return BufferedHandler<Handler>(a, h);
}

