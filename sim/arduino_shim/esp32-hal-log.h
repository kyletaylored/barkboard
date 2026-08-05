// Sim-only stub for ESP32 Arduino core's logging macros. Vendored WString.cpp
// calls log_e()/log_w() on allocation failure / replace() overflow — on the
// desktop simulator these just go to stderr instead of the ESP32 log system.
#pragma once
#include <cstdio>

#define log_e(fmt, ...) fprintf(stderr, "[E] " fmt "\n", ##__VA_ARGS__)
#define log_w(fmt, ...) fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#define log_i(fmt, ...) fprintf(stderr, "[I] " fmt "\n", ##__VA_ARGS__)
#define log_d(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
