#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <iostream>

void init_logger() {
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/app.log", true);

        // Set levels for each sink
        console_sink->set_level(spdlog::level::info);
        file_sink->set_level(spdlog::level::debug);

        // Create a logger with both sinks
        auto logger = std::make_shared<spdlog::logger>(
            "multi_sink", spdlog::sinks_init_list{ console_sink, file_sink });

        logger->set_level(spdlog::level::debug);
        logger->set_pattern("%Y-%m-%d %H:%M:%S.%e | %^%l%$ | %v");

        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::warn); // Flush file logs on warn or higher
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log init failed: " << ex.what() << std::endl;
    }
}
