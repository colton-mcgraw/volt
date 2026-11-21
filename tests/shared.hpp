#pragma once

#include <cstddef>
#include <chrono>
#include <string>
#include <functional>
#include <iostream>

namespace volt::tests
{

    bool expect(bool condition, const std::string &message)
    {
        if (!condition)
        {
            std::cerr << "[json-test] FAIL: " << message << '\n';
            return false;
        }
        return true;
    }

    struct ThroughputStats
    {
        std::size_t totalBytes{0};
        double totalTimeSeconds{0.0};

        [[nodiscard]] double bytesPerSecond() const
        {
            return totalTimeSeconds > 0.0 ? static_cast<double>(totalBytes) / totalTimeSeconds : 0.0;
        }
    };

    struct Benchmark
    {

        using time_point = std::chrono::high_resolution_clock::time_point;
        using duration = std::chrono::duration<double>;
        using Func = std::function<void(const std::byte *buffer, std::size_t size)>;

        [[nodiscard]] static duration measureTime(
            Func func,
            const std::byte *buffer,
            std::size_t size,
            std::size_t iterations = 1)
        {
            const auto startTime = std::chrono::high_resolution_clock::now();
            for (std::size_t i = 0; i < iterations; ++i)
            {
                func(buffer, size);
            }
            const auto endTime = std::chrono::high_resolution_clock::now();
            return endTime - startTime;
        }

        [[nodiscard]] static ThroughputStats measureThroughput(
            Func func,
            const std::byte *buffer,
            std::size_t size,
            std::size_t iterations = 1)
        {
            const auto duration = measureTime(func, buffer, size, iterations);
            return ThroughputStats{size * iterations, duration.count()};
        }

        [[nodiscard]] static std::string formatBytesPerSecond(double bytesPerSecond)
        {
            constexpr const char *units[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
            std::size_t unitIndex = 0;
            while (bytesPerSecond >= 1024.0 && unitIndex < std::size(units) - 1)
            {
                bytesPerSecond /= 1024.0;
                ++unitIndex;
            }
            return std::to_string(bytesPerSecond) + " " + units[unitIndex];
        }

        static void warmup(
            Func func,
            const std::byte *buffer,
            std::size_t size,
            std::size_t iterations = 3)
        {
            for (std::size_t i = 0; i < iterations; ++i)
            {
                func(buffer, size);
            }
        }
    };

}