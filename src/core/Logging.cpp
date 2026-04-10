#include "volt/core/Logging.hpp"

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <array>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <cctype>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
constexpr std::size_t kCategoryCount = 7;
}

namespace volt::core::logging {
namespace {

struct LoggingState {
  bool initialized{false};
  bool eventTraceEnabled{false};
  bool tickTraceEnabled{false};
  spdlog::level::level_enum configuredLevel{spdlog::level::info};
  std::array<bool, kCategoryCount> enabledCategories{true, true, true, true, true, true, true};
};

LoggingState g_state{};

[[nodiscard]] const char* tryGetEnv(const char* name) {
#if defined(_MSC_VER)
  static thread_local std::string envBuffer;
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    if (value != nullptr) {
      free(value);
    }
    return nullptr;
  }

  envBuffer.assign(value);
  free(value);
  return envBuffer.c_str();
#else
  return std::getenv(name);
#endif
}

[[nodiscard]] bool parseBool(const char* value, bool fallback) {
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }

  const char first = value[0];
  if (first == '1' || first == 't' || first == 'T' || first == 'y' || first == 'Y') {
    return true;
  }

  if (first == '0' || first == 'f' || first == 'F' || first == 'n' || first == 'N') {
    return false;
  }

  return fallback;
}

[[nodiscard]] spdlog::level::level_enum parseLevel(
    const char* value,
    spdlog::level::level_enum fallback) {
  if (value == nullptr) {
    return fallback;
  }

  const std::string level{value};
  if (level == "trace") {
    return spdlog::level::trace;
  }
  if (level == "debug") {
    return spdlog::level::debug;
  }
  if (level == "info") {
    return spdlog::level::info;
  }
  if (level == "warn") {
    return spdlog::level::warn;
  }
  if (level == "error") {
    return spdlog::level::err;
  }
  if (level == "critical") {
    return spdlog::level::critical;
  }

  return fallback;
}

[[nodiscard]] std::string normalizeToken(std::string token) {
  token.erase(
      std::remove_if(token.begin(), token.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
      }),
      token.end());

  std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  return token;
}

[[nodiscard]] std::size_t categoryIndex(Category category) {
  switch (category) {
    case Category::kCore:
      return 0;
    case Category::kApp:
      return 1;
    case Category::kPlatform:
      return 2;
    case Category::kRender:
      return 3;
    case Category::kUI:
      return 4;
    case Category::kIO:
      return 5;
    case Category::kEvent:
      return 6;
    default:
      return 0;
  }
}

void configureCategoryFilter(const char* value) {
  g_state.enabledCategories.fill(true);
  if (value == nullptr || value[0] == '\0') {
    return;
  }

  g_state.enabledCategories.fill(false);
  std::string raw{value};
  for (char& ch : raw) {
    if (ch == ';' || ch == '\t' || ch == '\n' || ch == '\r') {
      ch = ',';
    }
  }

  std::size_t start = 0;
  while (start < raw.size()) {
    while (start < raw.size() && (raw[start] == ',' || std::isspace(static_cast<unsigned char>(raw[start])) != 0)) {
      ++start;
    }
    if (start >= raw.size()) {
      break;
    }

    std::size_t end = start;
    while (end < raw.size() && raw[end] != ',' && std::isspace(static_cast<unsigned char>(raw[end])) == 0) {
      ++end;
    }

    const std::string token = normalizeToken(raw.substr(start, end - start));
    if (token.empty()) {
      start = end + 1;
      continue;
    }

    if (token == "all") {
      g_state.enabledCategories.fill(true);
      return;
    }
    if (token == "none") {
      g_state.enabledCategories.fill(false);
      return;
    }
    if (token == "core") {
      g_state.enabledCategories[categoryIndex(Category::kCore)] = true;
    } else if (token == "app") {
      g_state.enabledCategories[categoryIndex(Category::kApp)] = true;
    } else if (token == "platform") {
      g_state.enabledCategories[categoryIndex(Category::kPlatform)] = true;
    } else if (token == "render") {
      g_state.enabledCategories[categoryIndex(Category::kRender)] = true;
    } else if (token == "ui") {
      g_state.enabledCategories[categoryIndex(Category::kUI)] = true;
    } else if (token == "io") {
      g_state.enabledCategories[categoryIndex(Category::kIO)] = true;
    } else if (token == "event") {
      g_state.enabledCategories[categoryIndex(Category::kEvent)] = true;
    }

    start = end + 1;
  }
}

[[nodiscard]] spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return spdlog::level::trace;
    case LogLevel::kDebug:
      return spdlog::level::debug;
    case LogLevel::kInfo:
      return spdlog::level::info;
    case LogLevel::kWarn:
      return spdlog::level::warn;
    case LogLevel::kError:
      return spdlog::level::err;
    case LogLevel::kCritical:
      return spdlog::level::critical;
    default:
      return spdlog::level::info;
  }
}

[[nodiscard]] std::string_view levelNameFromSpdlog(spdlog::level::level_enum level) {
  switch (level) {
    case spdlog::level::trace:
      return "trace";
    case spdlog::level::debug:
      return "debug";
    case spdlog::level::info:
      return "info";
    case spdlog::level::warn:
      return "warn";
    case spdlog::level::err:
      return "error";
    case spdlog::level::critical:
      return "critical";
    default:
      return "info";
  }
}

}  // namespace

std::string_view categoryName(Category category) {
  switch (category) {
    case Category::kCore:
      return "core";
    case Category::kApp:
      return "app";
    case Category::kPlatform:
      return "platform";
    case Category::kRender:
      return "render";
    case Category::kUI:
      return "ui";
    case Category::kIO:
      return "io";
    case Category::kEvent:
      return "event";
    default:
      return "core";
  }
}

std::string_view configuredLevelName() {
#if defined(VOLT_ENABLE_LOGGING) && VOLT_ENABLE_LOGGING
  return levelNameFromSpdlog(g_state.configuredLevel);
#else
  return "off";
#endif
}

std::string enabledCategoriesSummary() {
#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  std::string summary;
  auto appendCategory = [&summary](Category category) {
    if (!summary.empty()) {
      summary.append(",");
    }
    summary.append(categoryName(category));
  };

  if (isCategoryEnabled(Category::kCore)) {
    appendCategory(Category::kCore);
  }
  if (isCategoryEnabled(Category::kApp)) {
    appendCategory(Category::kApp);
  }
  if (isCategoryEnabled(Category::kPlatform)) {
    appendCategory(Category::kPlatform);
  }
  if (isCategoryEnabled(Category::kRender)) {
    appendCategory(Category::kRender);
  }
  if (isCategoryEnabled(Category::kUI)) {
    appendCategory(Category::kUI);
  }
  if (isCategoryEnabled(Category::kIO)) {
    appendCategory(Category::kIO);
  }
  if (isCategoryEnabled(Category::kEvent)) {
    appendCategory(Category::kEvent);
  }

  if (summary.empty()) {
    return "none";
  }

  return summary;
#else
  return "none";
#endif
}

void initialize() {
#if defined(VOLT_ENABLE_LOGGING) && VOLT_ENABLE_LOGGING
  if (g_state.initialized) {
    return;
  }

  std::vector<spdlog::sink_ptr> sinks;

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  std::filesystem::create_directories("logs");
  sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/volt-debug.log", true));
#if defined(_WIN32)
  sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
  if (GetConsoleWindow() != nullptr) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }
#else
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
#else
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif

  auto logger = std::make_shared<spdlog::logger>("volt", sinks.begin(), sinks.end());
  spdlog::set_default_logger(logger);
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    g_state.configuredLevel = parseLevel(
    tryGetEnv("VOLT_LOG_LEVEL"),
  #if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
    spdlog::level::trace
  #else
    spdlog::level::info
  #endif
    );
  spdlog::set_level(g_state.configuredLevel);
  spdlog::flush_on(spdlog::level::warn);

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
    g_state.eventTraceEnabled = parseBool(tryGetEnv("VOLT_EVENT_TRACE"), true);
    g_state.tickTraceEnabled = parseBool(tryGetEnv("VOLT_TICK_TRACE"), true);
  configureCategoryFilter(tryGetEnv("VOLT_LOG_CATEGORIES"));
#else
  g_state.eventTraceEnabled = false;
  g_state.tickTraceEnabled = false;
  g_state.configuredLevel = spdlog::level::off;
  g_state.enabledCategories.fill(false);
#endif

  g_state.initialized = true;
#endif
}

void shutdown() {
#if defined(VOLT_ENABLE_LOGGING) && VOLT_ENABLE_LOGGING
  if (!g_state.initialized) {
    return;
  }

  spdlog::shutdown();
  g_state.initialized = false;
  g_state.eventTraceEnabled = false;
  g_state.tickTraceEnabled = false;
  g_state.configuredLevel = spdlog::level::off;
  g_state.enabledCategories.fill(false);
#endif
}

bool isFeatureEnabled(Feature feature) {
#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  switch (feature) {
    case Feature::kEventTrace:
      return g_state.eventTraceEnabled;
    case Feature::kTickTrace:
      return g_state.tickTraceEnabled;
    default:
      return false;
  }
#else
  (void)feature;
  return false;
#endif
}

void setFeatureEnabled(Feature feature, bool enabled) {
#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  switch (feature) {
    case Feature::kEventTrace:
      g_state.eventTraceEnabled = enabled;
      break;
    case Feature::kTickTrace:
      g_state.tickTraceEnabled = enabled;
      break;
    default:
      break;
  }
#else
  (void)feature;
  (void)enabled;
#endif
}

bool isCategoryEnabled(Category category) {
#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  return g_state.enabledCategories[categoryIndex(category)];
#else
  (void)category;
  return false;
#endif
}

void setCategoryEnabled(Category category, bool enabled) {
#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  g_state.enabledCategories[categoryIndex(category)] = enabled;
#else
  (void)category;
  (void)enabled;
#endif
}

void log(Category category, LogLevel level, const std::string& message) {
#if defined(VOLT_ENABLE_LOGGING) && VOLT_ENABLE_LOGGING
  if (!g_state.initialized) {
    initialize();
  }

#if defined(VOLT_ENABLE_DEBUG_LOGGING) && VOLT_ENABLE_DEBUG_LOGGING
  if (!isCategoryEnabled(category)) {
    return;
  }
#endif

  std::string categorizedMessage{"["};
  categorizedMessage.append(categoryName(category));
  categorizedMessage.append("] ");
  categorizedMessage.append(message);
  spdlog::log(toSpdlogLevel(level), categorizedMessage);
#else
  (void)category;
  (void)level;
  (void)message;
#endif
}

void log(LogLevel level, const std::string& message) {
#if defined(VOLT_ENABLE_LOGGING) && VOLT_ENABLE_LOGGING
  log(Category::kCore, level, message);
#else
  (void)level;
  (void)message;
#endif
}

}  // namespace volt::core::logging
