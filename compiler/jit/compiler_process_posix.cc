#include "compiler/jit/compiler_process.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

// fdopen 属于 POSIX C API，<cstdio> 不提供该声明。
extern "C" {
#include <stdio.h>
}

namespace vela::compiler::jit {

std::expected<std::shared_ptr<CompilerProcess>, std::string> CompilerProcess::Start(
    const std::filesystem::path& executable) {
  std::array<int, 2> request{};
  std::array<int, 2> reply{};
  if (pipe(request.data()) != 0) {
    return std::unexpected(std::strerror(errno));
  }
  if (pipe(reply.data()) != 0) {
    close(request[0]);
    close(request[1]);
    return std::unexpected(std::strerror(errno));
  }
  posix_spawn_file_actions_t actions{};
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, request[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, reply[1], STDOUT_FILENO);
  for (const auto descriptor : {request[0], request[1], reply[0], reply[1]}) {
    posix_spawn_file_actions_addclose(&actions, descriptor);
  }
  auto path = executable.string();
  std::string command = "compile-server";
  const std::array<char*, 3> arguments{path.data(), command.data(), nullptr};
#ifdef __APPLE__
  auto** environment = *_NSGetEnviron();
#else
  auto** environment = environ;
#endif
  decltype(getpid()) child = 0;
  const auto error = posix_spawn(&child, path.c_str(), &actions, nullptr, arguments.data(),
                                 environment);
  posix_spawn_file_actions_destroy(&actions);
  close(request[0]);
  close(reply[1]);
  if (error != 0) {
    close(request[1]);
    close(reply[0]);
    return std::unexpected(std::strerror(error));
  }
  CompilerStreams streams;
  streams.input.reset(::fdopen(request[1], "wb"));
  streams.output.reset(::fdopen(reply[0], "rb"));
  if (!streams.input) {
    close(request[1]);
  }
  if (!streams.output) {
    close(reply[0]);
  }
  return FinishStart(std::move(streams), static_cast<std::uintptr_t>(child));
}

CompilerProcess::~CompilerProcess() {
  CloseStreams();
  while (waitpid(static_cast<decltype(getpid())>(process_), nullptr, 0) < 0 && errno == EINTR) {
  }
}

}  // namespace vela::compiler::jit
