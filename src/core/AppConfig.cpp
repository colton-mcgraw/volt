#include "volt/core/AppConfig.hpp"

#include "volt/core/Logging.hpp"
#include "volt/io/assets/Manifest.hpp"

#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <optional>
#include <string>

namespace volt::core {

namespace {

bool equalsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs) {
	if (lhs.size() != rhs.size()) {
		return false;
	}

	for (std::size_t i = 0U; i < lhs.size(); ++i) {
		const unsigned char l = static_cast<unsigned char>(lhs[i]);
		const unsigned char r = static_cast<unsigned char>(rhs[i]);
		if (std::tolower(l) != std::tolower(r)) {
			return false;
		}
	}

	return true;
}

std::optional<std::string> findString(const volt::io::KeyValueManifest& manifest, const char* key) {
	const auto record = manifest.find(key);
	if (!record.has_value() ||
		record->kind != volt::io::ManifestRecord::ValueKind::kString ||
		record->value.empty()) {
		return std::nullopt;
	}
	return record->value;
}

std::optional<bool> findBool(const volt::io::KeyValueManifest& manifest, const char* key) {
	const auto record = manifest.find(key);
	if (!record.has_value()) {
		return std::nullopt;
	}

	if (record->kind == volt::io::ManifestRecord::ValueKind::kBoolean) {
		return equalsAsciiCaseInsensitive(record->value, "true") || record->value == "1";
	}

	if (record->kind == volt::io::ManifestRecord::ValueKind::kString) {
		if (equalsAsciiCaseInsensitive(record->value, "true") || record->value == "1") {
			return true;
		}
		if (equalsAsciiCaseInsensitive(record->value, "false") || record->value == "0") {
			return false;
		}
	}

	return std::nullopt;
}

std::optional<double> findNumber(const volt::io::KeyValueManifest& manifest, const char* key) {
	const auto record = manifest.find(key);
	if (!record.has_value()) {
		return std::nullopt;
	}

	if (record->kind != volt::io::ManifestRecord::ValueKind::kNumber &&
		record->kind != volt::io::ManifestRecord::ValueKind::kString) {
		return std::nullopt;
	}

	double parsed = 0.0;
	const char* begin = record->value.data();
	const char* end = record->value.data() + record->value.size();
	const auto [ptr, ec] = std::from_chars(begin, end, parsed);
	if (ec != std::errc{} || ptr != end) {
		return std::nullopt;
	}

	return parsed;
}

std::uint32_t parseWindowExtentValue(
		const volt::io::KeyValueManifest& manifest,
		const char* key,
		std::uint32_t fallback) {
	const auto value = findNumber(manifest, key);
	if (!value.has_value()) {
		return fallback;
	}

	if (*value <= 0.0) {
		return 0U;
	}

	return static_cast<std::uint32_t>(std::llround(*value));
}

std::int32_t parseWindowCoordinateValue(
		const volt::io::KeyValueManifest& manifest,
		const char* key,
		std::int32_t fallback) {
	const auto value = findNumber(manifest, key);
	if (!value.has_value()) {
		return fallback;
	}

	if (*value <= 0.0) {
		return -1;
	}

	if (*value > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
		return std::numeric_limits<std::int32_t>::max();
	}

	return static_cast<std::int32_t>(std::llround(*value));
}

std::int32_t parsePositiveIntOrDefault(
		const volt::io::KeyValueManifest& manifest,
		const char* key,
		std::int32_t fallback,
		std::int32_t nonPositiveValue) {
	const auto value = findNumber(manifest, key);
	if (!value.has_value()) {
		return fallback;
	}

	if (*value <= 0.0) {
		return nonPositiveValue;
	}

	if (*value > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
		return std::numeric_limits<std::int32_t>::max();
	}

	return static_cast<std::int32_t>(std::llround(*value));
}

}  // namespace

AppConfig::AppConfig() {
	volt::io::KeyValueManifest& manifest = volt::io::manifestService();
	manifest.refresh(false);
	if (manifest.isDisabled()) {
		return;
	}

	if (const auto value = findString(manifest, "appname"); value.has_value()) {
		appName = *value;
	}

	windowTitle = appName;
	if (const auto value = findString(manifest, "default-window-title"); value.has_value()) {
		windowTitle = *value;
	}

	if (const auto value = findBool(manifest, "window-resizable"); value.has_value()) {
		windowResizable = *value;
	}

	if (const auto value = findString(manifest, "window-icon"); value.has_value()) {
		windowIconPath = *value;
	} else if (const auto iconValue = findString(manifest, "icon"); iconValue.has_value()) {
		windowIconPath = *iconValue;
	}

	windowWidth = parseWindowExtentValue(manifest, "default-window-width", windowWidth);
	windowHeight = parseWindowExtentValue(manifest, "default-window-height", windowHeight);
	windowX = parseWindowCoordinateValue(manifest, "default-window-x", windowX);
	windowY = parseWindowCoordinateValue(manifest, "default-window-y", windowY);

	windowResizeBorderThickness = parsePositiveIntOrDefault(
			manifest,
			"window-resize-border-thickness",
			windowResizeBorderThickness,
			0);

	windowTitleBarHeight = parsePositiveIntOrDefault(
			manifest,
			"window-titlebar-height",
			windowTitleBarHeight,
			windowTitleBarHeight);

	windowTitleBarPadding = parsePositiveIntOrDefault(
			manifest,
			"window-titlebar-padding",
			windowTitleBarPadding,
			windowTitleBarPadding);

	windowTitleBarButtonSize = parsePositiveIntOrDefault(
			manifest,
			"window-titlebar-button-size",
			windowTitleBarButtonSize,
			windowTitleBarButtonSize);

	windowTitleBarButtonSpacing = parsePositiveIntOrDefault(
			manifest,
			"window-titlebar-button-spacing",
			windowTitleBarButtonSpacing,
			windowTitleBarButtonSpacing);
}

}  // namespace volt::core
