// neighbor() is never called. If it shows up as a sampled leaf, the resolver attributed a sample to the previous function.
__attribute__((noinline)) long neighbor(long x) { return x * 7 + 3; }
__attribute__((noinline)) long tiny(long x) { return x * 3 + 1; }   // 2 instructions, so samples often land on its first one
int main() {
    volatile long acc = 0;
    for (long i = 0; ; i++) acc = tiny(acc + i);
}
