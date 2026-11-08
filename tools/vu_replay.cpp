#include "runtime/ps2_vu1.h"
#include <cstdio>
#include <exception>
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: dra2-vu-replay capture.vu\n");
        return 2;
    }
    try
    {
        return VU1Interpreter::replayCapture(argv[1]);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
