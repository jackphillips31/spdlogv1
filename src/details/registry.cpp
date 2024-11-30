// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#include "spdlog/details/registry.h"

#include "spdlog/common.h"
#include "spdlog/logger.h"
#include "spdlog/pattern_formatter.h"

#ifndef SPDLOG_DISABLE_DEFAULT_LOGGER
    // support for the default stdout color logger
    #ifdef _WIN32
        #include "spdlog/sinks/wincolor_sink.h"
    #else

        #include "spdlog/sinks/ansicolor_sink.h"

    #endif
#endif  // SPDLOG_DISABLE_DEFAULT_LOGGER

#include <memory>
#include <string>
#include <unordered_map>

static constexpr size_t small_map_threshold = 10;

namespace spdlog {
namespace details {

registry::registry()
    : formatter_(new pattern_formatter()) {
#ifndef SPDLOG_DISABLE_DEFAULT_LOGGER
    // create default logger (ansicolor_stdout_sink_mt or wincolor_stdout_sink_mt in windows).
    #ifdef _WIN32
    auto color_sink = std::make_shared<sinks::wincolor_stdout_sink_mt>();
    #else
    auto color_sink = std::make_shared<sinks::ansicolor_stdout_sink_mt>();
    #endif
    const char *default_logger_name = "";
    default_logger_ = std::make_shared<spdlog::logger>(default_logger_name, std::move(color_sink));
    loggers_[default_logger_name] = default_logger_;

#endif  // SPDLOG_DISABLE_DEFAULT_LOGGER
}

registry::~registry() = default;

void registry::register_logger(std::shared_ptr<logger> new_logger) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    register_logger_(std::move(new_logger));
}

void registry::initialize_logger(std::shared_ptr<logger> new_logger) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    new_logger->set_formatter(formatter_->clone());

    if (err_handler_) {
        new_logger->set_error_handler(err_handler_);
    }

    // set new level according to previously configured level or default level
    auto it = log_levels_.find(new_logger->name());
    auto new_level = it != log_levels_.end() ? it->second : global_log_level_;
    new_logger->set_level(new_level);

    new_logger->flush_on(flush_level_);

    if (automatic_registration_) {
        register_logger_(std::move(new_logger));
    }
}

// if the map is small do a sequential search, otherwise use the standard find()
std::shared_ptr<logger> registry::get(const std::string &logger_name) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    if (loggers_.size() <= small_map_threshold) {
        for (const auto &[key, val] : loggers_) {
            if (logger_name == key) {
                return val;
            }
        }
        return nullptr;
    }
    auto found = loggers_.find(logger_name);
    return found == loggers_.end() ? nullptr : found->second;
}

// if the map is small do a sequential search and avoid creating string for find(logger_name)
// otherwise use the standard find()
std::shared_ptr<logger> registry::get(std::string_view logger_name) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    if (loggers_.size() <= small_map_threshold) {
        for (const auto &[key, val] : loggers_) {
            if (logger_name == key) {
                return val;
            }
        }
        return nullptr;
    }
    // otherwise use the normal map lookup
    const auto found = loggers_.find(std::string(logger_name));
    return found == loggers_.end() ? nullptr : found->second;
}

std::shared_ptr<logger> registry::get(const char *logger_name) { return get(std::string_view(logger_name)); }

std::shared_ptr<logger> registry::default_logger() {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    return default_logger_;
}

// Return raw ptr to the default logger.
// To be used directly by the spdlog default api (e.g. spdlog::info)
// This make the default API faster, but cannot be used concurrently with set_default_logger().
// e.g do not call set_default_logger() from one thread while calling spdlog::info() from another.
logger *registry::get_default_raw() { return default_logger_.get(); }

// set default logger.
// default logger is stored in default_logger_ (for faster retrieval) and in the loggers_ map.
void registry::set_default_logger(std::shared_ptr<logger> new_default_logger) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    if (new_default_logger != nullptr) {
        loggers_[new_default_logger->name()] = new_default_logger;
    }
    default_logger_ = std::move(new_default_logger);
}

void registry::set_tp(std::shared_ptr<thread_pool> tp) {
    std::lock_guard<std::recursive_mutex> lock(tp_mutex_);
    tp_ = std::move(tp);
}

std::shared_ptr<thread_pool> registry::get_tp() {
    std::lock_guard<std::recursive_mutex> lock(tp_mutex_);
    return tp_;
}

// Set global formatter. Each sink in each logger will get a clone of this object
void registry::set_formatter(std::unique_ptr<formatter> formatter) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    formatter_ = std::move(formatter);
    for (auto &l : loggers_) {
        l.second->set_formatter(formatter_->clone());
    }
}

void registry::set_level(level level) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    for (auto &l : loggers_) {
        l.second->set_level(level);
    }
    global_log_level_ = level;
}

void registry::flush_on(level level) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    for (auto &l : loggers_) {
        l.second->flush_on(level);
    }
    flush_level_ = level;
}

void registry::set_error_handler(err_handler handler) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    for (auto &l : loggers_) {
        l.second->set_error_handler(handler);
    }
    err_handler_ = std::move(handler);
}

void registry::apply_all(const std::function<void(const std::shared_ptr<logger>)> &fun) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    for (auto &l : loggers_) {
        fun(l.second);
    }
}

void registry::flush_all() {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    for (auto &l : loggers_) {
        l.second->flush();
    }
}

void registry::drop(const std::string &logger_name) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    auto is_default_logger = default_logger_ && default_logger_->name() == logger_name;
    loggers_.erase(logger_name);
    if (is_default_logger) {
        default_logger_.reset();
    }
}

void registry::drop_all() {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    loggers_.clear();
    default_logger_.reset();
}

// clean all resources and threads started by the registry
void registry::shutdown() {
    {
        std::lock_guard<std::mutex> lock(flusher_mutex_);
        periodic_flusher_.reset();
    }

    drop_all();

    {
        std::lock_guard<std::recursive_mutex> lock(tp_mutex_);
        tp_.reset();
    }
}

std::recursive_mutex &registry::tp_mutex() { return tp_mutex_; }

void registry::set_automatic_registration(bool automatic_registration) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    automatic_registration_ = automatic_registration;
}

void registry::set_levels(log_levels levels, level *global_level) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    log_levels_ = std::move(levels);
    auto global_level_requested = global_level != nullptr;
    global_log_level_ = global_level_requested ? *global_level : global_log_level_;

    for (auto &logger : loggers_) {
        auto logger_entry = log_levels_.find(logger.first);
        if (logger_entry != log_levels_.end()) {
            logger.second->set_level(logger_entry->second);
        } else if (global_level_requested) {
            logger.second->set_level(*global_level);
        }
    }
}

registry &registry::instance() {
    static registry s_instance;
    return s_instance;
}

void registry::apply_logger_env_levels(std::shared_ptr<logger> new_logger) {
    std::lock_guard<std::mutex> lock(logger_map_mutex_);
    auto it = log_levels_.find(new_logger->name());
    auto new_level = it != log_levels_.end() ? it->second : global_log_level_;
    new_logger->set_level(new_level);
}

void registry::throw_if_exists_(const std::string &logger_name) {
    if (loggers_.find(logger_name) != loggers_.end()) {
        throw_spdlog_ex("logger with name '" + logger_name + "' already exists");
    }
}

void registry::register_logger_(std::shared_ptr<logger> new_logger) {
    auto logger_name = new_logger->name();
    throw_if_exists_(logger_name);
    loggers_[logger_name] = std::move(new_logger);
}

}  // namespace details
}  // namespace spdlog
