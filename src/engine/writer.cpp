#include "../include/writer.h"
#include "engineprivate.h"
#include <libfilezilla/local_filesys.hpp>

writer_factory_holder::writer_factory_holder(writer_factory_holder const& op)
{
	if (op.impl_) {
		impl_ = op.impl_->clone();
	}
}

writer_factory_holder& writer_factory_holder::operator=(writer_factory_holder const& op)
{
	if (this != &op && op.impl_) {
		impl_ = op.impl_->clone();
	}
	return *this;
}

writer_factory_holder::writer_factory_holder(writer_factory_holder && op) noexcept
{
	impl_ = std::move(op.impl_);
	op.impl_.reset();
}

writer_factory_holder& writer_factory_holder::operator=(writer_factory_holder && op) noexcept
{
	if (this != &op) {
		impl_ = std::move(op.impl_);
		op.impl_.reset();
	}

	return *this;
}

writer_factory_holder::writer_factory_holder(std::unique_ptr<writer_factory> && factory)
	: impl_(std::move(factory))
{
}

writer_factory_holder::writer_factory_holder(std::unique_ptr<writer_factory> const& factory)
	: impl_(factory ? factory->clone() : nullptr)
{
}


writer_factory_holder::writer_factory_holder(writer_factory const& factory)
	: impl_(factory.clone())
{
}


writer_factory_holder& writer_factory_holder::operator=(std::unique_ptr<writer_factory> && factory)
{
	if (impl_ != factory) {
		impl_ = std::move(factory);
	}

	return *this;
}

file_writer_factory::file_writer_factory(std::wstring const& file, bool fsync)
	: writer_factory(file)
	, fsync_(fsync)
{
}

std::unique_ptr<writer_factory> file_writer_factory::clone() const
{
	return std::make_unique<file_writer_factory>(*this);
}

uint64_t file_writer_factory::size() const
{
	if (size_) {
		return *size_;
	}
	auto s = fz::local_filesys::get_size(fz::to_native(name_));
	if (s < 0) {
		size_ = npos;
	}
	else {
		size_ = static_cast<uint64_t>(s);
	}
	return *size_;
}

std::unique_ptr<writer_base> file_writer_factory::open(uint64_t offset, CFileZillaEnginePrivate & engine, fz::event_handler & handler, aio_base::shm_flag shm)
{
	auto ret = std::make_unique<file_writer>(name_, engine, handler);

	if (ret->open(offset, fsync_, shm) != aio_result::ok) {
		ret.reset();
	}

	return ret;
}

namespace {
void remove_writer_events(fz::event_handler * handler, writer_base const* writer)
{
	if (!handler) {
		return;
	}
	auto event_filter = [&](fz::event_loop::Events::value_type const& ev) -> bool {
		if (ev.first != handler) {
			return false;
		}
		else if (ev.second->derived_type() == write_ready_event::type()) {
			return std::get<0>(static_cast<write_ready_event const&>(*ev.second).v_) == writer;
		}
		return false;
	};

	handler->event_loop_.filter_events(event_filter);
}
}

writer_base::writer_base(std::wstring const& name, CFileZillaEnginePrivate & engine, fz::event_handler & handler)
	: aio_base(name, engine, handler)
{}

void writer_base::close()
{
	ready_count_ = 0;
	
	remove_writer_events(&handler_, this);
}

get_write_buffer_result writer_base::get_write_buffer(fz::nonowning_buffer & last_written)
{
	fz::scoped_lock l(mtx_);
	if (error_) {
		return {aio_result::error, fz::nonowning_buffer()};
	}

	if (processing_ && last_written) {
		buffers_[(ready_pos_ + ready_count_) % buffers_.size()] = last_written;
		bool signal = !ready_count_;
		++ready_count_;
		if (signal) {
			signal_capacity(l);
		}
	}
	last_written.reset();
	if (ready_count_ >= buffers_.size()) {
		handler_waiting_ = true;
		processing_ = false;
		return {aio_result::wait, fz::nonowning_buffer()};
	}
	else {
		processing_ = true;
		auto b = buffers_[(ready_pos_ + ready_count_) % buffers_.size()];
		b.resize(0);
		return {aio_result::ok, b};
	}
}

aio_result writer_base::retire(fz::nonowning_buffer & last_written)
{
	fz::scoped_lock l(mtx_);
	if (error_) {
		return aio_result::error;
	}

	if (!processing_) {
		return aio_result::error;
	}
	processing_ = false;
	if (last_written) {
		buffers_[(ready_pos_ + ready_count_) % buffers_.size()] = last_written;
		bool signal = !ready_count_;
		++ready_count_;
		if (signal) {
			signal_capacity(l);
		}
	}
	last_written.reset();
	return {aio_result::ok};
}

aio_result writer_base::write(uint8_t* data, size_t len)
{
	fz::scoped_lock l(mtx_);
	if (error_ || processing_) {
		return aio_result::error;
	}

	if (!len) {
		 return aio_result::ok;
	}

	if (ready_count_ >= buffers_.size()) {
		handler_waiting_ = true;
		return aio_result::wait;
	}

	auto & b = buffers_[(ready_pos_ + ready_count_) % buffers_.size()];

	len = std::min(len, buffer_size_);
	memcpy(b.get(len), data, len);
	b.add(len);

	bool signal = !ready_count_;
	++ready_count_;
	if (signal) {
		signal_capacity(l);
	}
	return aio_result::ok;
}

aio_result writer_base::finalize(fz::nonowning_buffer & last_written)
{
	fz::scoped_lock l(mtx_);
	if (error_) {
		return aio_result::error;
	}
	if (processing_ && last_written) {
		buffers_[(ready_pos_ + ready_count_) % buffers_.size()] = last_written;
		last_written.reset();
		processing_ = false;
		bool signal = !ready_count_;
		++ready_count_;
		if (signal) {
			signal_capacity(l);
		}
	}
	if (ready_count_) {
		handler_waiting_ = true;
		return aio_result::wait;
	}

	auto res = continue_finalize();
	if (res == aio_result::ok) {
		finalized_ = true;
	}
	return res;
}

file_writer::file_writer(std::wstring const& name, CFileZillaEnginePrivate & engine, fz::event_handler & handler)
	: writer_base(name, engine, handler)
{
}

file_writer::~file_writer()
{
	close();
}

void file_writer::close()
{
	{
		fz::scoped_lock l(mtx_);
		quit_ = true;
		cond_.signal(l);
	}

	thread_.join();

	writer_base::close();
}

aio_result file_writer::open(uint64_t offset, bool fsync, shm_flag shm)
{
	fsync_ = fsync;

	if (!allocate_memory(shm)) {
		return aio_result::error;
	}

	std::wstring tmp;
	CLocalPath local_path(name_, &tmp);
	if (local_path.HasParent()) {
		fz::native_string last_created;
		fz::mkdir(fz::to_native(local_path.GetPath()), true, false, &last_created);
		if (!last_created.empty()) {
			// Send out notification
			auto n = std::make_unique<CLocalDirCreatedNotification>();
			if (n->dir.SetPath(fz::to_wstring(last_created))) {
				engine_.AddNotification(std::move(n));
			}
		}
	}

	if (!file_.open(fz::to_native(name_), fz::file::writing, offset ? fz::file::existing : fz::file::empty)) {
		return aio_result::error;
	}

	if (offset) {
		auto const ofs = static_cast<int64_t>(offset);
		if (file_.seek(ofs, fz::file::begin) != ofs) {
			return aio_result::error;
		}
		if (!file_.truncate()) {
			return aio_result::error;
		}
	}

	thread_ = engine_.GetThreadPool().spawn([this]() { entry(); });
	if (!thread_) {
		return aio_result::error;
	}

	return aio_result::ok;
}

void file_writer::entry()
{
	fz::scoped_lock l(mtx_);
	while (!quit_ && !error_) {
		if (!ready_count_) {
			if (handler_waiting_) {
				handler_waiting_ = false;
				handler_.send_event<write_ready_event>(this);
				break;
			}

			cond_.wait(l);
			continue;
		}

		fz::nonowning_buffer & b = buffers_[ready_pos_];

		while (!b.empty()) {
			l.unlock();
			auto written = file_.write(b.get(), b.size());
			l.lock();
			if (quit_) {
				return;
			}
			if (written > 0) {
				b.consume(static_cast<size_t>(written));
			}
			else {
				error_ = true;
				break;
			}
		}

		ready_pos_ = (ready_pos_ + 1) % buffers_.size();
		--ready_count_;

		if (handler_waiting_) {
			handler_waiting_ = false;
			handler_.send_event<write_ready_event>(this);
		}
	}
}

void file_writer::signal_capacity(fz::scoped_lock & l)
{
	cond_.signal(l);
}

uint64_t file_writer::size() const
{
	fz::scoped_lock l(mtx_);
	if (size_) {
		return *size_;
	}
	auto s = file_.size();
	if (s < 0) {
		size_ = nosize;
	}
	else {
		size_ = static_cast<uint64_t>(s);
	}
	return *size_;
}

aio_result file_writer::continue_finalize()
{
	if (fsync_) {
		if (!file_.fsync()) {
			error_ = true;
			return aio_result::error;
		}
	}
	return aio_result::ok;
}


#include <libfilezilla/buffer.hpp>

memory_writer_factory::memory_writer_factory(std::wstring const& name, fz::buffer & result_buffer, size_t sizeLimit)
	: writer_factory(name)
	, result_buffer_(&result_buffer)
	, sizeLimit_(sizeLimit)
{
}

std::unique_ptr<writer_factory> memory_writer_factory::clone() const
{
	return std::make_unique<memory_writer_factory>(*this);
}

std::unique_ptr<writer_base> memory_writer_factory::open(uint64_t offset, CFileZillaEnginePrivate & engine, fz::event_handler & handler, aio_base::shm_flag shm)
{
	if (!result_buffer_ || offset) {
		return nullptr;
	}

	auto ret = std::make_unique<memory_writer>(name_, engine, handler, *result_buffer_, sizeLimit_);
	if (ret->open(shm) != aio_result::ok) {
		ret.reset();
	}

	return ret;
}

memory_writer::memory_writer(std::wstring const& name, CFileZillaEnginePrivate & engine, fz::event_handler & handler, fz::buffer & result_buffer, size_t sizeLimit)
	: writer_base(name, engine, handler)
	, result_buffer_(result_buffer)
	, sizeLimit_(sizeLimit)
{}

memory_writer::~memory_writer()
{
	close();
}

void memory_writer::close()
{
	if (!finalized_) {
		result_buffer_.clear();
	}
}

aio_result memory_writer::open(shm_flag shm)
{
	result_buffer_.clear();
	if (!allocate_memory(shm)) {
		return aio_result::error;
	}
	
	return aio_result::ok;
}

uint64_t memory_writer::size() const
{
	fz::scoped_lock l(mtx_);
	return result_buffer_.size();
}

void memory_writer::signal_capacity(fz::scoped_lock & l)
{
	--ready_count_;
	auto & b = buffers_[ready_pos_];
	if (sizeLimit_ && b.size() > sizeLimit_ - result_buffer_.size()) {
		error_ = true;
	}
	else {
		result_buffer_.append(b.get(), b.size());
	}
}
