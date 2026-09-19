#include <exception>
#include <iostream>

#include "path_execution.h"
#include "path_input.h"
#include "path_report.h"

int main(int argc, char **argv) {
    try {
        const PathOptions options = parse_options(argc, argv);
        const PathSchedule schedule = load_schedule(options);
        RunnerContext context = initialize_runner(options);
        const auto warmup_samples = run_samples(schedule.warmup_leaves, options.height, context);
        const auto samples = run_samples(schedule.measured_leaves, options.height, context);
        write_report(options, schedule, context, warmup_samples, samples);
    } catch (const std::exception &e) {
        std::cerr << "vbpir_path_adapter: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
