#pragma once

#include <sstream>
#include <string_view>
#include <string>
#include <utility>

namespace volt::core::logging {

enum class Feature {
  kEventTrace,
  kTickTrace,
};

enum class LogLevel {
  kTrace,
  kDebug,
  kInfo,
  kWarn,
  kError,
  kCritical,
};

enum class Category {
  kCore,
  kApp,
  kPlatform,
  kRender,
  kUI,
  kIO,
  kEvent,
};

void initialize();
void shutdown();

[[nodiscard]] bool isFeatureEnabled(Feature feature);
void setFeatureEnabled(Feature feature, bool enabled);

[[nodiscard]] bool isCategoryEnabled(Category category);
void setCategoryEnabled(Category category, bool enabled);

[[nodiscard]] bool shouldLog(Category category, LogLevel level);
[[nodiscard]] bool shouldLog(LogLevel level);

[[nodiscard]] std::string_view categoryName(Category category);
[[nodiscard]] std::string_view configuredLevelName();
[[nodiscard]] std::string enabledCategoriesSummary();

void log(Category category, LogLevel level, const std::string& message);
void log(LogLevel level, const std::string& message);

namespace detail {

template <typename... Args>
std::string buildMessage(Args&&... args) {
  std::ostringstream stream;
  (stream << ... << std::forward<Args>(args));
  return stream.str();
}

}  // namespace detail

}  // namespace volt::core::logging

#if defined(VOLT_ENABLE_LOGGING) && VOLT_ENABLE_LOGGING
#define VOLT_LOG_INFO_CAT(category, ...) \
  do { \
    if (::volt::core::logging::shouldLog(category, ::volt::core::logging::LogLevel::kInfo)) { \
      ::volt::core::logging::log(\
          category,\
          ::volt::core::logging::LogLevel::kInfo,\
          ::volt::core::logging::detail::buildMessage(__VA_ARGS__)); \
    } \
  } while (0)
#define VOLT_LOG_WARN_CAT(category, ...) \
  do { \
    if (::volt::core::logging::shouldLog(category, ::volt::core::logging::LogLevel::kWarn)) { \
      ::volt::core::logging::log(\
          category,\
          ::volt::core::logging::LogLevel::kWarn,\
          ::volt::core::logging::detail::buildMessage(__VA_ARGS__)); \
    } \
  } while (0)
#define VOLT_LOG_ERROR_CAT(category, ...) \
  do { \
    if (::volt::core::logging::shouldLog(category, ::volt::core::logging::LogLevel::kError)) { \
      ::volt::core::logging::log(\
          category,\
          ::volt::core::logging::LogLevel::kError,\
          ::volt::core::logging::detail::buildMessage(__VA_ARGS__)); \
    } \
  } while (0)
#define VOLT_LOG_CRITICAL_CAT(category, ...) \
  do { \
    if (::volt::core::logging::shouldLog(category, ::volt::core::logging::LogLevel::kCritical)) { \
      ::volt::core::logging::log(\
          category,\
          ::volt::core::logging::LogLevel::kCritical,\
          ::volt::core::logging::detail::buildMessage(__VA_ARGS__)); \
    } \
  } while (0)

#define VOLT_LOG_INFO(...) \
  VOLT_LOG_INFO_CAT(::volt::core::logging::Category::kCore, __VA_ARGS__)
#define VOLT_LOG_WARN(...) \
  VOLT_LOG_WARN_CAT(::volt::core::logging::Category::kCore, __VA_ARGS__)
#define VOLT_LOG_ERROR(...) \
  VOLT_LOG_ERROR_CAT(::volt::core::logging::Category::kCore, __VA_ARGS__)
#define VOLT_LOG_CRITICAL(...) \
  VOLT_LOG_CRITICAL_CAT(::volt::core::logging::Category::kCore, __VA_ARGS__)
#else
#define VOLT_LOG_INFO_CAT(category, ...) ((void)0)
#define VOLT_LOG_WARN_CAT(category, ...) ((void)0)
#define VOLT_LOG_ERROR_CAT(category, ...) ((void)0)
#define VOLT_LOG_CRITICAL_CAT(category, ...) ((void)0)
#define VOLT_LOG_INFO(...) ((void)0)
#define VOLT_LOG_WARN(...) ((void)0)
#define VOLT_LOG_ERROR(...) ((void)0)
#define VOLT_LOG_CRITICAL(...) ((void)0)
#endif

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
#define VOLT_LOG_TRACE_CAT(category, ...) \
  do { \
    if (::volt::core::logging::shouldLog(category, ::volt::core::logging::LogLevel::kTrace)) { \
      ::volt::core::logging::log(\
          category,\
          ::volt::core::logging::LogLevel::kTrace,\
          ::volt::core::logging::detail::buildMessage(__VA_ARGS__)); \
    } \
  } while (0)
#define VOLT_LOG_DEBUG_CAT(category, ...) \
  do { \
    if (::volt::core::logging::shouldLog(category, ::volt::core::logging::LogLevel::kDebug)) { \
      ::volt::core::logging::log(\
          category,\
          ::volt::core::logging::LogLevel::kDebug,\
          ::volt::core::logging::detail::buildMessage(__VA_ARGS__)); \
    } \
  } while (0)

#define VOLT_LOG_TRACE(...) \
  VOLT_LOG_TRACE_CAT(::volt::core::logging::Category::kCore, __VA_ARGS__)
#define VOLT_LOG_DEBUG(...) \
  VOLT_LOG_DEBUG_CAT(::volt::core::logging::Category::kCore, __VA_ARGS__)
#else
#define VOLT_LOG_TRACE_CAT(category, ...) ((void)0)
#define VOLT_LOG_DEBUG_CAT(category, ...) ((void)0)
#define VOLT_LOG_TRACE(...) ((void)0)
#define VOLT_LOG_DEBUG(...) ((void)0)
#endif
