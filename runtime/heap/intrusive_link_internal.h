#pragma once

namespace vela::runtime::internal {

// 节点借用所属容器的头指针；成员指针在有权访问节点布局的调用边界提供。
template <class T>
void PrependIntrusive(T& node, T*& first, T* T::* next_member, T** T::* previous_member) noexcept {
  auto& next = node.*next_member;
  next = first;
  node.*previous_member = &first;
  if (next != nullptr) {
    next->*previous_member = &next;
  }
  first = &node;
}

template <class T>
void UnlinkIntrusive(T& node, T* T::* next_member, T** T::* previous_member) noexcept {
  auto& previous = node.*previous_member;
  if (previous == nullptr) {
    return;
  }
  auto& next = node.*next_member;
  *previous = next;
  if (next != nullptr) {
    next->*previous_member = previous;
  }
  next = nullptr;
  previous = nullptr;
}

}  // namespace vela::runtime::internal
