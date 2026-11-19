#pragma once

#include <memory>

namespace vela::runtime {

class Isolate;

// Isolate 及宿主拥有的数据必须覆盖挂接作用域；挂接不保活脚本值，也不替代 GC 根。
// 同类型的嵌套挂接遮蔽外层值；所有访问和析构都在 Isolate 所属线程进行。
class HostData final {
 public:
  template <class T>
  HostData(Isolate& isolate, T& value) noexcept : key_(&kKey<T>), value_(std::addressof(value)) {
    Attach(isolate);
  }
  ~HostData();
  HostData(const HostData&) = delete;
  HostData& operator=(const HostData&) = delete;
  HostData(HostData&&) = delete;
  HostData& operator=(HostData&&) = delete;

  template <class T>
  [[nodiscard]] static T* Find(const Isolate& isolate) noexcept {
    return static_cast<T*>(Find(isolate, &kKey<T>));
  }

 private:
  template <class T>
  inline static const char kKey = 0;

  void Attach(Isolate& isolate) noexcept;
  [[nodiscard]] static void* Find(const Isolate& isolate, const void* key) noexcept;

  const void* key_;
  void* value_;
  HostData* next_ = nullptr;
  HostData** previous_link_ = nullptr;
};

}  // namespace vela::runtime
