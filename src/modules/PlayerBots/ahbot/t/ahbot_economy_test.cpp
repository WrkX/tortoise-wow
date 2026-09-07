#include "AhBotEconomy.h"

#include <cstdio>
#include <string>

int main()
{
    std::string errors;
    if (!ahbot::SelfCheck(errors))
    {
        std::fprintf(stderr, "ahbot economy self-check failed: %s\n", errors.c_str());
        return 1;
    }

    std::printf("ahbot economy self-check passed\n");
    return 0;
}
