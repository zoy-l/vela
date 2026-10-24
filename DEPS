vars = {
  "abseil_revision": "76bb24329e8bf5f39704eb10d21b9a80befa7c81",
  "benchmark_revision": "192ef10025eb2c4cdd392bc502f0c852196baa48",
  "googletest_revision": "52eb8108c5bdec04579160ae17225d66034bd723",
  "clang_revision": "git_revision:80743bd43fd5b38fedc503308e7a652e23d3ec93",
  "gn_windows_amd64": "oSpjjlooOXrPOilIB3Jp8v8m7oWgj_1FzsBmUS0uyf4C",
  "gn_linux_amd64": "HGFL8feSRdxEbidQY0WQp34I18nDUSi0hBC_CpgshIEC",
  "gn_mac_arm64": "h4iAdH4LUYD6MwCSJ2oNIcfSzoakAe--qmJ4HILxLI4C",
  "ninja_windows_amd64": "3TgJ1Ckw_8bqhqyrAzltNPV1oYhZClrVWwXWMVE8RVgC",
  "ninja_linux_amd64": "YUD4fWqXb4Xz0OauVp4OjmQvyyXPNg2dQoW6_V9DtPUC",
  "ninja_mac_arm64": "CDfXaGzqupkn-D7OYSq_rPX-J5UfnidZKASuyrvGOtEC",
}

deps = {
  "third_party/abseil-cpp": "https://github.com/abseil/abseil-cpp.git@" + Var("abseil_revision"),
  "third_party/benchmark": "https://github.com/google/benchmark.git@" + Var("benchmark_revision"),
  "third_party/googletest": "https://github.com/google/googletest.git@" + Var("googletest_revision"),
  "third_party/build_tools/clang/windows-amd64": {
    "packages": [{
      "package": "fuchsia/third_party/clang/windows-amd64",
      "version": Var("clang_revision"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'win' and host_cpu == 'x64'",
  },
  "third_party/build_tools/clang/linux-amd64": {
    "packages": [{
      "package": "fuchsia/third_party/clang/linux-amd64",
      "version": Var("clang_revision"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'linux' and host_cpu == 'x64'",
  },
  "third_party/build_tools/clang/mac-arm64": {
    "packages": [{
      "package": "fuchsia/third_party/clang/mac-arm64",
      "version": Var("clang_revision"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'mac' and host_cpu == 'arm64'",
  },
  "third_party/build_tools/gn/windows-amd64": {
    "packages": [{
      "package": "gn/gn/windows-amd64",
      "version": Var("gn_windows_amd64"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'win' and host_cpu == 'x64'",
  },
  "third_party/build_tools/gn/linux-amd64": {
    "packages": [{
      "package": "gn/gn/linux-amd64",
      "version": Var("gn_linux_amd64"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'linux' and host_cpu == 'x64'",
  },
  "third_party/build_tools/gn/mac-arm64": {
    "packages": [{
      "package": "gn/gn/mac-arm64",
      "version": Var("gn_mac_arm64"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'mac' and host_cpu == 'arm64'",
  },
  "third_party/build_tools/ninja/windows-amd64": {
    "packages": [{
      "package": "infra/3pp/tools/ninja/windows-amd64",
      "version": Var("ninja_windows_amd64"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'win' and host_cpu == 'x64'",
  },
  "third_party/build_tools/ninja/linux-amd64": {
    "packages": [{
      "package": "infra/3pp/tools/ninja/linux-amd64",
      "version": Var("ninja_linux_amd64"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'linux' and host_cpu == 'x64'",
  },
  "third_party/build_tools/ninja/mac-arm64": {
    "packages": [{
      "package": "infra/3pp/tools/ninja/mac-arm64",
      "version": Var("ninja_mac_arm64"),
    }],
    "dep_type": "cipd",
    "condition": "host_os == 'mac' and host_cpu == 'arm64'",
  },
}
