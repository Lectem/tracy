#include <string>
__attribute__((noinline)) long work(const std::string& s) {
    volatile long x = 0;
    for (long i = 0; i < 300000000; i++) x += (long)s.size();
    return x;
}
__attribute__((noinline)) void caller(int n) {
    // a temporary std::string is built for the call and destroyed right after it returns (inlined ctor/dtor)
    work(std::string("a string long enough to need heap storage, not the small string buffer") + std::to_string(n));
}
int main() { for (int i = 0; ; i++) caller(i); }
