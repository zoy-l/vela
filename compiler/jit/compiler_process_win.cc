#include "compiler/jit/compiler_process.h"

#include <windows.h>  // IWYU pragma: keep

#include <basetsd.h>
#include <corecrt_io.h>
#include <errhandlingapi.h>
#include <fcntl.h>
#include <fileapi.h>
#include <handleapi.h>
#include <minwinbase.h>
#include <minwindef.h>
#include <namedpipeapi.h>
#include <processenv.h>
#include <processthreadsapi.h>
#include <synchapi.h>
#include <winbase.h>
#include <winerror.h>
#include <winnt.h>
#include <utility>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// _fdopen 是 CRT 的 C ABI 扩展，声明不属于 C++ std 命名空间。
extern "C" {
#include <stdio.h>
}

namespace vela::compiler::jit {

namespace {
std::expected<HANDLE, std::string> CreateDiagnosticsHandle(SECURITY_ATTRIBUTES& security) {
  HANDLE diagnostic = nullptr;
  auto* const inherited_error = GetStdHandle(STD_ERROR_HANDLE);
  if (inherited_error != nullptr && inherited_error != INVALID_HANDLE_VALUE) {
    if (DuplicateHandle(GetCurrentProcess(), inherited_error, GetCurrentProcess(), &diagnostic, 0,
                        TRUE, DUPLICATE_SAME_ACCESS) == 0) {
      return std::unexpected("cannot duplicate compiler diagnostics handle");
    }
  } else {
    diagnostic = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (diagnostic == INVALID_HANDLE_VALUE) {
      return std::unexpected("cannot open compiler diagnostics handle");
    }
  }

  return diagnostic;
}
}  // namespace

std::expected<std::shared_ptr<CompilerProcess>, std::string> CompilerProcess::Start(
    const std::filesystem::path& executable) {
  SECURITY_ATTRIBUTES security{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
  };
  HANDLE read_request = nullptr;
  HANDLE write_request = nullptr;
  HANDLE read_reply = nullptr;
  HANDLE write_reply = nullptr;
  const auto close_pipes = [&] {
    for (auto* const handle : {read_request, write_request, read_reply, write_reply}) {
      if (handle != nullptr) {
        CloseHandle(handle);
      }
    }
  };
  if ((CreatePipe(&read_request, &write_request, &security, 0) == 0) ||
      (CreatePipe(&read_reply, &write_reply, &security, 0) == 0)) {
    close_pipes();
    return std::unexpected("cannot create compiler pipes");
  }
  SetHandleInformation(write_request, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(read_reply, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = read_request;
  startup.StartupInfo.hStdOutput = write_reply;
  const auto diagnostics = CreateDiagnosticsHandle(security);
  if (!diagnostics) {
    close_pipes();
    return std::unexpected(diagnostics.error());
  }
  auto* diagnostic = *diagnostics;
  startup.StartupInfo.hStdError = diagnostic;
  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  std::vector<std::byte> storage(attribute_bytes);
  void* attribute_storage = storage.data();
  startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage);
  if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes) == 0) {
    CloseHandle(diagnostic);
    close_pipes();
    return std::unexpected("cannot initialize compiler handle list");
  }
  std::array handles{read_request, write_reply, startup.StartupInfo.hStdError};
  if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                static_cast<void*>(handles.data()), sizeof(handles), nullptr,
                                nullptr) == 0) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(diagnostic);
    close_pipes();
    return std::unexpected("cannot set compiler handle list");
  }
  auto command = L"\"" + executable.wstring() + L"\" compile-server";
  PROCESS_INFORMATION process{};
  const auto created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                      nullptr, &startup.StartupInfo, &process);
  const auto creation_error = (created != 0) ? ERROR_SUCCESS : GetLastError();
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  CloseHandle(diagnostic);
  if (created == 0) {
    close_pipes();
    return std::unexpected("cannot start compiler process: Win32 " +
                           std::to_string(creation_error));
  }
  CloseHandle(process.hThread);
  CloseHandle(read_request);
  CloseHandle(write_reply);
  const auto input_fd = _open_osfhandle(std::bit_cast<std::intptr_t>(write_request), _O_BINARY);
  const auto output_fd = _open_osfhandle(std::bit_cast<std::intptr_t>(read_reply), _O_BINARY);
  CompilerStreams streams;
  streams.input.reset(input_fd < 0 ? nullptr : _fdopen(input_fd, "wb"));
  streams.output.reset(output_fd < 0 ? nullptr : _fdopen(output_fd, "rb"));
  if (!streams.input || !streams.output) {
    if (!streams.input && input_fd >= 0) {
      _close(input_fd);
    } else if (!streams.input) {
      CloseHandle(write_request);
    }
    if (!streams.output && output_fd >= 0) {
      _close(output_fd);
    } else if (!streams.output) {
      CloseHandle(read_reply);
    }
    streams.input.reset();
    streams.output.reset();
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hProcess);
    return std::unexpected("cannot open compiler streams");
  }
  return std::make_shared<CompilerProcess>(std::move(streams), process.hProcess);
}

CompilerProcess::~CompilerProcess() {
  CloseStreams();
  auto* const process = process_;
  WaitForSingleObject(process, INFINITE);
  CloseHandle(process);
}

}  // namespace vela::compiler::jit
