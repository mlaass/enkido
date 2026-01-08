#include <iostream>
#include <cstdlib>
#include "cedar/cedar.hpp"

void print_usage(const char* program) {
    std::cout << "Cedar Synth Engine v" << cedar::Version::string() << "\n\n"
              << "Usage: " << program << " [options] <bytecode-file>\n\n"
              << "Options:\n"
              << "  -h, --help       Show this help message\n"
              << "  -v, --version    Show version information\n"
              << "  -r, --rate <hz>  Set sample rate (default: 48000)\n"
              << "  -b, --block <n>  Set block size (default: 128)\n"
              << std::endl;
}

void print_version() {
    std::cout << "cedar " << cedar::Version::string() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }

        if (arg == "-v" || arg == "--version") {
            print_version();
            return EXIT_SUCCESS;
        }
    }

    // Initialize Cedar
    cedar::Config config{};
    if (!cedar::init(config)) {
        std::cerr << "error: failed to initialize Cedar\n";
        return EXIT_FAILURE;
    }

    std::cout << "Cedar initialized (sample rate: " << cedar::config().sample_rate
              << " Hz, block size: " << cedar::config().block_size << ")\n";

    // TODO: Load bytecode and run audio loop
    std::cout << "Bytecode execution not yet implemented\n";

    cedar::shutdown();
    return EXIT_SUCCESS;
}
