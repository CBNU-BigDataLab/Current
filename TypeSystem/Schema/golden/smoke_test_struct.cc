// The `current.h` file is the one from `https://github.com/C5T/Current`.
// Compile with `-std=c++11` or higher.

#include "current.h"

// clang-format off

namespace current_userspace {
struct Primitives {
  // It's the order of fields that matters.
  uint8_t a;

  // Field descriptions can be set in any order.
  uint16_t b;
  uint32_t c;
  uint64_t d;
  int8_t e;
  int16_t f;
  int32_t g;
  int64_t h;
  char i;
  std::string j;
  float k;
  double l;

  // Multiline
  // descriptions
  // can be used.
  bool m;
  std::chrono::microseconds n;
  std::chrono::milliseconds o;
};
struct A {
  int32_t a;
};
struct B : A {
  int32_t b;
};
struct B2 : A {
};
struct Empty {
};
struct X {
  int32_t x;
};
enum class E : uint16_t {};
struct Y {
  E e;
};
using MyFreakingVariant = Variant<A, X, Y>;
struct C {
  Empty e;
  MyFreakingVariant c;
};
using Variant_B_A_B_B2_C_Empty_E = Variant<A, B, B2, C, Empty>;
struct FullTest {
  // A structure with a lot of primitive types.
  Primitives primitives;
  std::vector<std::string> v1;
  std::vector<Primitives> v2;
  std::pair<std::string, Primitives> p;
  Optional<Primitives> o;

  // Field | descriptions | FTW !
  Variant_B_A_B_B2_C_Empty_E q;
};
}  // namespace current_userspace

// clang-format on
