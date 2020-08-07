// This executable is only for testing purposes.
// We use it to see if we can find the function symbols and debug information.

#include <unistd.h>
#include <iostream>

struct ABCStruct {
  int a;
  int b;
  int c;
};

// Using extern C to avoid name mangling (which just keeps the test a bit more readable).
extern "C" {
int CanYouFindThis(int a, int b) { return a + b; }
ABCStruct SomeFunction(ABCStruct x, ABCStruct y) { return ABCStruct{x.a+y.a, x.b+y.b, x.c+y.c}; }
void SomeFunctionWithPointerArgs(int* a, ABCStruct* x) { x->a = *a; a++; }
}

namespace pl {
namespace testing {

class Foo {
 public:
  int Bar(int i) const {
    return i * i;
  }
};

}  // namespace testing
}  // namespace pl

int main() {
  for (int i=0; true; ++i) {
    int sum = CanYouFindThis(3, 4);
    std::cout << sum << std::endl;

    ABCStruct struct_sum = SomeFunction(ABCStruct{1, 2, 3}, ABCStruct{4, 5, 6});
    std::cout << struct_sum.a << std::endl;

    pl::testing::Foo foo;
    std::cout << foo.Bar(3) << std::endl;

    sleep(1);
  }

  return 0;
}
