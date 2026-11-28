#include "compiler/jit/compiler_process.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "compiler/frontend/modules/source_progress.h"
#include "compiler/jit/compiler_protocol.h"

namespace vela::compiler::jit {

CompilerProcess::CompilerProcess(CompilerStreams streams, CompilerProcessHandle process)
    : streams_(std::move(streams)), process_(process) {}

std::expected<std::shared_ptr<CompilerProcess>, std::string> CompilerProcess::FinishStart(
    CompilerStreams streams,
    CompilerProcessHandle process) {
  auto owner = std::make_shared<CompilerProcess>(std::move(streams), process);
  if (!owner->streams_.input || !owner->streams_.output) {
    return std::unexpected("cannot open compiler streams");
  }
  return owner;
}

std::expected<std::vector<std::byte>, std::string> CompilerProcess::Receive() const {
  return ReadCompilerFrame(streams_.output.get());
}

void CompilerProcess::StartReceiving() {
  receiver_ = std::thread([this] { ReadMessages(); });
}

void CompilerProcess::SetFileChanged(std::uint64_t resource_id, std::function<void()> changed) {
  const std::scoped_lock lock(callbacks_mutex_);
  if (changed) {
    callbacks_.insert_or_assign(resource_id, std::move(changed));
  } else {
    callbacks_.erase(resource_id);
  }
}

std::expected<void, std::string> CompilerProcess::QueueProgress(std::span<const std::byte> bytes) {
  BinaryReader reader(bytes);
  const auto stage = reader.Read<std::uint8_t>();
  auto path = reader.Text();
  const auto counts = reader.Record<std::uint64_t, std::uint64_t>();
  if (!stage || *stage > static_cast<std::uint8_t>(frontend::SourceProgressKind::Loading) ||
      !path || !counts || reader.remaining() != 0) {
    return std::unexpected("invalid compiler source progress");
  }
  const auto [completed, total] = *counts;
  const auto kind = static_cast<frontend::SourceProgressKind>(*stage);
  if (completed > total ||
      (kind == frontend::SourceProgressKind::File && (completed == 0 || path->empty()))) {
    return std::unexpected("invalid compiler source progress");
  }
  {
    const std::scoped_lock lock(response_mutex_);
    progress_.push_back({
        .kind = kind,
        .path = std::move(*path),
        .completed = static_cast<std::size_t>(completed),
        .total = static_cast<std::size_t>(total),
    });
  }
  response_ready_.notify_one();
  return {};
}

void CompilerProcess::ReadMessages() {
  std::string failure;
  for (;;) {
    auto message = Receive();
    if (!message) {
      failure = std::move(message.error());
      break;
    }
    if (message->empty()) {
      failure = "invalid compiler message";
      break;
    }
    // 消息类型在帧尾，响应缓冲直接转移，不复制或移动整份目标码。
    const auto kind = static_cast<CompilerMessage>(std::to_integer<std::uint8_t>(message->back()));
    message->pop_back();
    if (kind == CompilerMessage::Response) {
      {
        const std::scoped_lock lock(response_mutex_);
        response_ = std::move(*message);
      }
      response_ready_.notify_one();
    } else if (kind == CompilerMessage::SourceProgress) {
      const auto queued = QueueProgress(*message);
      if (!queued) {
        failure = queued.error();
        break;
      }
    } else if (kind == CompilerMessage::FilesChanged) {
      BinaryReader reader(*message);
      const auto resource_id = reader.Read<std::uint64_t>();
      if (!resource_id || reader.remaining() != 0) {
        failure = "invalid compiler file notification";
        break;
      }
      const std::scoped_lock lock(callbacks_mutex_);
      const auto found = callbacks_.find(*resource_id);
      if (found != callbacks_.end()) {
        found->second();
      }
    } else {
      failure = "unknown compiler message";
      break;
    }
  }
  {
    const std::scoped_lock lock(response_mutex_);
    channel_ = std::unexpected(std::move(failure));
  }
  response_ready_.notify_all();
  const std::scoped_lock lock(callbacks_mutex_);
  for (const auto& [resource_id, changed] : callbacks_) {
    changed();
  }
}

void CompilerProcess::Release(CompilerCommand command, std::uint64_t resource_id) {
  const auto released = Exchange(CompilerRequest(command, resource_id).bytes());
  if (!released) {
    std::println(stderr, "Vela compiler release: {}", released.error());
  }
}

void CompilerProcess::CloseStreams() {
  streams_.input.reset();
  if (receiver_.joinable()) {
    receiver_.join();
  }
  streams_.output.reset();
}

std::expected<std::vector<std::byte>, std::string> CompilerProcess::Exchange(
    std::span<const std::byte> request,
    const frontend::SourceProgressCallback& progress) {
  const std::scoped_lock lock(mutex_);
  {
    const std::scoped_lock response_lock(response_mutex_);
    if (!channel_) {
      return std::unexpected(channel_.error());
    }
  }
  if (auto written = WriteCompilerFrame(streams_.input.get(), request); !written) {
    return std::unexpected(std::move(written.error()));
  }
  std::unique_lock response_lock(response_mutex_);
  for (;;) {
    response_ready_.wait(response_lock,
                         [this] { return response_ || !progress_.empty() || !channel_; });
    auto pending = std::move(progress_);
    progress_.clear();
    response_lock.unlock();
    if (progress) {
      for (const auto& event : pending) {
        progress({
            .kind = event.kind,
            .path = event.path,
            .completed = event.completed,
            .total = event.total,
        });
      }
    }
    response_lock.lock();
    if (progress_.empty() && (response_ || !channel_)) {
      break;
    }
  }
  if (!response_) {
    // wait 已保证收到响应或通道失败；这里直接返回通道的终态错误。
    return std::unexpected(channel_.error());
  }
  auto response = std::move(*response_);
  response_.reset();
  response_lock.unlock();
  if (response.empty()) {
    return std::unexpected("invalid compiler response");
  }
  const auto success = std::to_integer<std::uint8_t>(response.back());
  response.pop_back();
  if (success > 1) {
    return std::unexpected("invalid compiler response");
  }
  if (success == 0) {
    BinaryReader reader(response);
    auto message = reader.Text();
    if (!message || reader.remaining() != 0) {
      return std::unexpected("invalid compiler error response");
    }
    return std::unexpected(std::move(*message));
  }
  return response;
}

}  // namespace vela::compiler::jit
