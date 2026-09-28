#include "common/version.hpp"

#include <iostream>

int main() {
    std::cout << "streamingasr " << sasr::version_string() << '\n';
    return 0;
}
