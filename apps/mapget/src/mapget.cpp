#include "mapget/http-service/cli.h"

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(argc);
    for (auto i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return mapget::runFromCommandLine(args);
}
