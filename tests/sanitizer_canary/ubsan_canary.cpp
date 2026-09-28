// Deliberate signed integer overflow. Must be reported by UBSan.
#include <climits>

int main(int argc, char**) {
    int x = INT_MAX;
    x += argc;  // argc >= 1, so this overflows
    return x == 0 ? 1 : 0;
}
