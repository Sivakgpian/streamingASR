// Deliberate heap-buffer-overflow. Must be reported by AddressSanitizer.
#include <vector>

int main() {
    std::vector<int> v(4, 0);
    const volatile int* p = v.data();
    return p[v.size()];  // one past the end
}
