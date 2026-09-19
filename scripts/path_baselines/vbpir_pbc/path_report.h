#ifndef VBPIR_PATH_REPORT_H
#define VBPIR_PATH_REPORT_H

#include <vector>

#include "path_execution.h"
#include "path_input.h"

void write_report(const PathOptions &options, const PathSchedule &schedule,
                  RunnerContext &context,
                  const std::vector<PathSample> &warmup_samples,
                  const std::vector<PathSample> &samples);

#endif
