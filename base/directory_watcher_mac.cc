#include "base/directory_watcher.h"

#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFBase.h>
#include <CoreFoundation/CFString.h>
#include <CoreServices/CoreServices.h>  // IWYU pragma: keep
#include <dispatch/dispatch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/directory_watcher_mac.h"
#include "base/path.h"

namespace vela {
namespace {

std::expected<std::filesystem::path, std::string> EventPath(CFStringRef value) {
  auto* normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, value);
  if (normalized == nullptr) {
    return std::unexpected("Cannot copy FSEvents path");
  }
  // 源码路径采用 UTF-8；统一原生文件名的分解形式，避免同一名字无法命中路径索引。
  CFStringNormalize(normalized, kCFStringNormalizationFormC);
  const auto capacity =
      CFStringGetMaximumSizeForEncoding(CFStringGetLength(normalized), kCFStringEncodingUTF8) + 1;
  std::string text(static_cast<std::size_t>(capacity), '\0');
  const auto converted =
      CFStringGetCString(normalized, text.data(), capacity, kCFStringEncodingUTF8);
  CFRelease(normalized);
  if (converted == 0U) {
    return std::unexpected("Cannot decode FSEvents path as UTF-8");
  }
  text.resize(text.find('\0'));
  return Utf8Path(text);
}

}  // namespace

namespace internal {

struct MacDirectoryWatch final {
  // callback 只访问不可变注册数据和 owner 的事件队列，不取得订阅锁。
  static void Events(ConstFSEventStreamRef /*stream*/,
                     void* context,
                     std::size_t count,
                     void* paths,
                     const FSEventStreamEventFlags* flags,
                     const FSEventStreamEventId* /*ids*/) {
    auto& watch = *static_cast<MacDirectoryWatch*>(context);
    const auto* names = static_cast<CFArrayRef>(paths);
    const std::span events(flags, count);
    std::vector<FileChange> changes;
    changes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto event = events[index];
      constexpr auto kLost =
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagMustScanSubDirs) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagUserDropped) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagKernelDropped) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagEventIdsWrapped) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagRootChanged) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagUnmount);
      if ((event & kLost) != 0U) {
        watch.owner->Invalidate();
        continue;
      }
      const auto* name =
          static_cast<CFStringRef>(CFArrayGetValueAtIndex(names, static_cast<CFIndex>(index)));
      auto decoded = EventPath(name);
      if (!decoded) {
        watch.owner->Invalidate();
        continue;
      }
      const auto relative = decoded->lexically_relative(watch.native_directory);
      if (relative.empty() || relative == "." || *relative.begin() == "..") {
        continue;
      }
      if (watch.scope == DirectoryWatchScope::Direct && relative.has_parent_path()) {
        continue;
      }
      constexpr auto kIdentity =
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemCreated) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemRemoved) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemRenamed);
      constexpr auto kContent =
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemModified) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemInodeMetaMod) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemFinderInfoMod) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemChangeOwner) |
          static_cast<FSEventStreamEventFlags>(kFSEventStreamEventFlagItemXattrMod);
      if ((event & (kIdentity | kContent)) == 0U) {
        continue;
      }
      changes.push_back({
          .path = (watch.directory / relative).lexically_normal(),
          .kind = (event & kIdentity) != 0U ? FileChangeKind::Identity : FileChangeKind::Content,
      });
    }
    watch.owner->Record(changes);
  }

  DirectoryWatcher* owner;
  std::filesystem::path directory;
  std::filesystem::path native_directory;
  DirectoryWatchScope scope;
  FSEventStreamRef stream = nullptr;
};

DirectoryWatchState::DirectoryWatchState() = default;
DirectoryWatchState::~DirectoryWatchState() = default;

}  // namespace internal

std::expected<void, std::string> DirectoryWatcher::AddNative(const std::filesystem::path& directory,
                                                             DirectoryWatchScope scope,
                                                             std::uint64_t subscription_id) {
  std::error_code error;
  const auto real_path = std::filesystem::canonical(directory, error);
  if (error) {
    return std::unexpected(error.message());
  }
  if (!std::filesystem::is_directory(real_path, error)) {
    return std::unexpected(error ? error.message() : "The watched path is not a directory");
  }
  const auto text = PathText(real_path);
  const auto* path =
      CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
  if (path == nullptr) {
    return std::unexpected("The watched directory is not a valid UTF-8 path");
  }
  auto native_directory = EventPath(path);
  if (!native_directory) {
    CFRelease(path);
    return std::unexpected(std::move(native_directory.error()));
  }
  auto watch = std::make_unique<internal::MacDirectoryWatch>(internal::MacDirectoryWatch{
      .owner = this,
      .directory = directory,
      .native_directory = std::move(*native_directory),
      .scope = scope,
  });
  std::array<const void*, 1> paths{path};
  const auto* array = CFArrayCreate(kCFAllocatorDefault, paths.data(),
                                    static_cast<CFIndex>(paths.size()), &kCFTypeArrayCallBacks);
  CFRelease(path);
  if (array == nullptr) {
    return std::unexpected("Cannot allocate FSEvents directory list");
  }
  FSEventStreamContext context{
      .version = 0,
      .info = watch.get(),
      .retain = nullptr,
      .release = nullptr,
      .copyDescription = nullptr,
  };
  constexpr auto kFlags =
      static_cast<FSEventStreamCreateFlags>(kFSEventStreamCreateFlagFileEvents) |
      static_cast<FSEventStreamCreateFlags>(kFSEventStreamCreateFlagNoDefer) |
      static_cast<FSEventStreamCreateFlags>(kFSEventStreamCreateFlagWatchRoot) |
      static_cast<FSEventStreamCreateFlags>(kFSEventStreamCreateFlagUseCFTypes);
  watch->stream = FSEventStreamCreate(kCFAllocatorDefault, &internal::MacDirectoryWatch::Events,
                                      &context, array, kFSEventStreamEventIdSinceNow, 0.0, kFlags);
  CFRelease(array);
  if (watch->stream == nullptr) {
    return std::unexpected("Cannot create FSEvents directory stream");
  }
  if (native_.queue == nullptr) {
    auto* queue = dispatch_queue_create("vela.directory_watch", DISPATCH_QUEUE_SERIAL);
    if (queue == nullptr) {
      FSEventStreamInvalidate(watch->stream);
      FSEventStreamRelease(watch->stream);
      return std::unexpected("Cannot create FSEvents dispatch queue");
    }
    native_.queue = queue;
  }
  auto* queue = static_cast<dispatch_queue_t>(native_.queue);
  FSEventStreamSetDispatchQueue(watch->stream, queue);
  if (FSEventStreamStart(watch->stream) == 0U) {
    FSEventStreamInvalidate(watch->stream);
    dispatch_sync_f(queue, nullptr, [](void*) {});
    FSEventStreamRelease(watch->stream);
    return std::unexpected("Cannot start FSEvents directory stream");
  }
  native_.watches.emplace(subscription_id, std::move(watch));
  return {};
}

void DirectoryWatcher::RemoveNative(std::uint64_t subscription_id) {
  const auto found = native_.watches.find(subscription_id);
  if (found == native_.watches.end()) {
    return;
  }
  auto* stream = found->second->stream;
  FSEventStreamStop(stream);
  FSEventStreamInvalidate(stream);
  // 最后一个已排队回调返回后，才归还其注册数据和 owner 借用。
  dispatch_sync_f(static_cast<dispatch_queue_t>(native_.queue), nullptr, [](void*) {});
  FSEventStreamRelease(stream);
  native_.watches.erase(found);
}

void DirectoryWatcher::CloseNative() {
  while (!native_.watches.empty()) {
    RemoveNative(native_.watches.begin()->first);
  }
  if (native_.queue != nullptr) {
    dispatch_release(static_cast<dispatch_queue_t>(native_.queue));
  }
}

}  // namespace vela
