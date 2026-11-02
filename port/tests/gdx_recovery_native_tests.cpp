#include <cstdio>
extern "C" int GdxRecoveryTest_Native(void);
int main() {
    const int result = GdxRecoveryTest_Native();
    std::printf("Native recovery fixture: %d\n", result);
    return result;
}
