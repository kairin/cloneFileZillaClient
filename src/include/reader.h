#ifndef FILEZILLA_ENGINE_IO_HEADER
#define FILEZILLA_ENGINE_IO_HEADER

#include "aio.h"

#include <libfilezilla/event.hpp>
#include <libfilezilla/event_handler.hpp>
#include <libfilezilla/file.hpp>
#include <libfilezilla/thread_pool.hpp>

#include <optional>

class reader_base;

struct read_ready_event_type{};
typedef fz::simple_event<read_ready_event_type, reader_base*> read_ready_event;

class FZC_PUBLIC_SYMBOL reader_factory
{
public:
	virtual ~reader_factory() noexcept = default;

	virtual std::unique_ptr<reader_factory> clone() = 0;

	// If shm_flag is valid, the buffers are allocated in shared memory suitable for communication with child processes
	// On Windows pass a bool, otherwise a valid file descriptor obtained by memfd_create or shm_open.
	virtual std::unique_ptr<reader_base> open(uint64_t offset, CFileZillaEnginePrivate & engine, fz::event_handler & handler, aio_base::shm_flag shm) = 0;

	virtual uint64_t size() const { return aio_base::nosize; }

protected:
	reader_factory() = default;
	reader_factory(reader_factory const&) = default;
	reader_factory& operator=(reader_factory const&) = default;
};

class FZC_PUBLIC_SYMBOL reader_factory_holder final
{
public:
	reader_factory_holder() = default;
	explicit reader_factory_holder(std::unique_ptr<reader_factory> && factory);

	reader_factory_holder(reader_factory_holder const& op);
	reader_factory_holder& operator=(reader_factory_holder const& op);

	reader_factory_holder(reader_factory_holder && op) noexcept;
	reader_factory_holder& operator=(reader_factory_holder && op) noexcept;
	reader_factory_holder& operator=(std::unique_ptr<reader_factory> && factory);

	std::unique_ptr<reader_base> open(uint64_t offset, CFileZillaEnginePrivate & engine, fz::event_handler & handler, aio_base::shm_flag shm)
	{
		return impl_ ? impl_->open(offset, engine, handler, shm) : nullptr;
	}

	uint64_t size() const
	{
		return impl_ ? impl_->size() : static_cast<uint64_t>(-1);
	}

	explicit operator bool() const { return impl_.operator bool(); }

private:
	std::unique_ptr<reader_factory> impl_;
};

class FZC_PUBLIC_SYMBOL file_reader_factory final : public reader_factory
{
public:
	file_reader_factory(std::wstring const& file);
	
	virtual std::unique_ptr<reader_base> open(uint64_t offset, CFileZillaEnginePrivate & engine, fz::event_handler & handler, aio_base::shm_flag shm) override;
	virtual std::unique_ptr<reader_factory> clone() override;

	virtual uint64_t size() const override;

	std::wstring file_;
	mutable std::optional<uint64_t> size_;
};

struct read_result {
	bool operator==(aio_result const t) const { return type_ == t; }

	aio_result type_{aio_result::error};

	// If type is ok and buffer is empty, we're at eof
	fz::nonowning_buffer buffer_;
};

class FZC_PUBLIC_SYMBOL reader_base : public aio_base
{
public:
	explicit reader_base(std::wstring const& name, CFileZillaEnginePrivate & engine, fz::event_handler & handler);

	virtual void close();

	// May be empty
	virtual std::wstring const& name() const = 0;

	virtual uint64_t size() const { return static_cast<uint64_t>(-1); }

	read_result read();
};

class FZC_PUBLIC_SYMBOL file_reader final : public reader_base
{
public:
	explicit file_reader(std::wstring const& name, CFileZillaEnginePrivate & engine, fz::event_handler & handler);
	~file_reader();

	virtual void close() override;

	virtual std::wstring const& name() const override { return name_; }
	virtual uint64_t size() const override;

protected:
	virtual void signal_capacity(fz::scoped_lock & l) override;

private:
	friend class file_reader_factory;
	aio_result open(uint64_t offset, shm_flag shm);

	void entry();

	fz::file file_;

	mutable std::optional<uint64_t> size_;

	fz::async_task thread_;
	fz::condition cond_;
};

#endif
