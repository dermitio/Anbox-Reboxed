#ifndef CORE_PROPERTY_H_
#define CORE_PROPERTY_H_

namespace core {
template <typename T>
class Property {
 public:
  Property() = default;
  explicit Property(const T& value) : value_{value} {}

  const T& get() const { return value_; }
  T& get() { return value_; }

  void set(const T& value) { value_ = value; }

  operator const T&() const { return value_; }

 private:
  T value_{};
};
}

#endif
