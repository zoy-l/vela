vars = {
  "luajit_revision": "a4c064539ea931274d407e1249b4bb575424a0f4",
  "abseil_revision": "76bb24329e8bf5f39704eb10d21b9a80befa7c81",
  "benchmark_revision": "192ef10025eb2c4cdd392bc502f0c852196baa48",
  "googletest_revision": "52eb8108c5bdec04579160ae17225d66034bd723",
  "utf8proc_revision": "79cdd5ab40c79afa559f689ffc13b76812dee1ac",
  "yyjson_revision": "6447536015f3d600f3d65323b10976103b337ca7",
  "xxhash_revision": "e626a72bc2321cd320e953a0ccf1584cad60f363",
  "pcre2_revision": "6f9d7c1373262c541324a16a358785b33ef116cf",
  "zstd_revision": "f8745da6ff1ad1e7bab384bd1f9d742439278e99",
  "clang_revision": "git_revision:80743bd43fd5b38fedc503308e7a652e23d3ec93",
  "llvm_sdk_version": "21.1.8",
  "llvm_sdk_license_sha256": "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee",
  "llvm_sdk_mac_arm64_archive": "LLVM-21.1.8-macOS-ARM64.tar.xz",
  "llvm_sdk_mac_arm64_sha256": "b95bdd32a33a81ee4d40363aaeb26728a26783fcef26a4d80f65457433ea4669",
  "llvm_sdk_linux_amd64_archive": "LLVM-21.1.8-Linux-X64.tar.xz",
  "llvm_sdk_linux_amd64_sha256": "b3b7f2801d15d50736acea3c73982994d025b01c2f035b91ae3b49d1b575732b",
  "llvm_sdk_windows_amd64_archive": "clang+llvm-21.1.8-x86_64-pc-windows-msvc.tar.xz",
  "llvm_sdk_windows_amd64_sha256": "749d22f565fcd5718dbed06512572d0e5353b502c03fe1f7f17ee8b8aca21a47",
  "gn_windows_amd64": "oSpjjlooOXrPOilIB3Jp8v8m7oWgj_1FzsBmUS0uyf4C",
  "gn_linux_amd64": "HGFL8feSRdxEbidQY0WQp34I18nDUSi0hBC_CpgshIEC",
  "gn_mac_arm64": "h4iAdH4LUYD6MwCSJ2oNIcfSzoakAe--qmJ4HILxLI4C",
  "ninja_windows_amd64": "3TgJ1Ckw_8bqhqyrAzltNPV1oYhZClrVWwXWMVE8RVgC",
  "ninja_linux_amd64": "YUD4fWqXb4Xz0OauVp4OjmQvyyXPNg2dQoW6_V9DtPUC",
  "ninja_mac_arm64": "CDfXaGzqupkn-D7OYSq_rPX-J5UfnidZKASuyrvGOtEC",
}

deps = {
  "third_party/luajit": "https://github.com/LuaJIT/LuaJIT.git@" + Var("luajit_revision"),
  "third_party/abseil-cpp": "https://github.com/abseil/abseil-cpp.git@" + Var("abseil_revision"),
  "third_party/benchmark": "https://github.com/google/benchmark.git@" + Var("benchmark_revision"),
  "third_party/googletest": "https://github.com/google/googletest.git@" + Var("googletest_revision"),
  "third_party/utf8proc": "https://github.com/JuliaStrings/utf8proc.git@" + Var("utf8proc_revision"),
  "third_party/yyjson": "https://github.com/ibireme/yyjson.git@" + Var("yyjson_revision"),
  "third_party/xxhash": "https://github.com/Cyan4973/xxHash.git@" + Var("xxhash_revision"),
  "third_party/pcre2": "https://github.com/PCRE2Project/pcre2.git@" + Var("pcre2_revision"),
  "third_party/zstd": "https://github.com/facebook/zstd.git@" + Var("zstd_revision"),
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

hooks = [{
  "name": "unicode_case_data",
  "pattern": ".",
  "action": ["python3", "tools/unicode_case.py", "--sync"],
}, {
  "name": "llvm_sdk",
  "pattern": ".",
  "action": ["python3", "-m", "tools.llvm", "sync"],
}]
