// Deliberate data race on a plain int. Must be reported by ThreadSanitizer.
#include <thread>

namespace {
int counter = 0;
}

int main() {
    std::thread t([] {
        for (int i = 0; i < 1000; ++i) ++counter;
    });
    for (int i = 0; i < 1000; ++i) ++counter;
    t.join();
    return counter == 0 ? 1 : 0;
}
