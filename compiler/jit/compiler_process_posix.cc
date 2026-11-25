#include "compiler/jit/compiler_process.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstring>

extern char** environ;

namespace vela::compiler::jit {

std::expected<std::shared_ptr<CompilerProcess>, std::string> CompilerProcess::Start(
    const std::filesystem::path& executable) {
  int request[2], reply[2];
  if (pipe(request) != 0)
    return std::unexpected(std::strerror(errno));
  if (pipe(reply) != 0) {
    close(request[0]);
    close(request[1]);
    return std::unexpected(std::strerror(errno));
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, request[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, reply[1], STDOUT_FILENO);
  for (const auto descriptor : {request[0], request[1], reply[0], reply[1]})
    posix_spawn_file_actions_addclose(&actions, descriptor);
  auto path = executable.string();
  char command[] = "compile-server";
  char* arguments[]{path.data(), command, nullptr};
  pid_t child = 0;
  const auto error = posix_spawn(&child, path.c_str(), &actions, nullptr, arguments, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(request[0]);
  close(reply[1]);
  if (error != 0) {
    close(request[1]);
    close(reply[0]);
    return std::unexpected(std::strerror(error));
  }
  CompilerStreams streams;
  streams.input.reset(fdopen(request[1], "wb"));
  streams.output.reset(fdopen(reply[0], "rb"));
  if (!streams.input || !streams.output) {
    if (!streams.input)
      close(request[1]);
    if (!streams.output)
      close(reply[0]);
    streams.input.reset();
    streams.output.reset();
    waitpid(child, nullptr, 0);
    return std::unexpected("cannot open compiler streams");
  }
  return std::make_shared<CompilerProcess>(std::move(streams), static_cast<std::uintptr_t>(child));
}

CompilerProcess::~CompilerProcess() {
  CloseStreams();
  while (waitpid(static_cast<pid_t>(process_), nullptr, 0) < 0 && errno == EINTR) {
  }
}

}  // namespace vela::compiler::jit
