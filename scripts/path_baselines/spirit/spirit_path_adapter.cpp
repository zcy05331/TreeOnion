#include "spirit_adapter_types.h"

#include <exception>
#include <iostream>

int main(int argc, char **argv)
{
    try {
        const spirit_path_adapter::Options options =
            spirit_path_adapter::parse_options(argc, argv);
        const spirit_path_adapter::RunResult result =
            spirit_path_adapter::run_spirit_path(options);
        spirit_path_adapter::write_output(
            options.output,
            spirit_path_adapter::render_json(options, result));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
