#pragma once

#include "volt/event/Event.hpp"
#include "volt/event/EventDispatcher.hpp"
#include "volt/io/image/ImageDecoder.hpp"
#include "volt/io/image/ImageEncoder.hpp"
#include "volt/io/import/ImporterRegistry.hpp"
#include "volt/io/import/ImportPipeline.hpp"
#include "volt/core/Logging.hpp"

namespace volt::app
{

    [[maybe_unused]] inline const char *eventTypeToString(volt::event::EventType type)
    {
        switch (type)
        {
        case volt::event::EventType::kFrameStarted:
            return "FrameStarted";
        case volt::event::EventType::kFrameEnded:
            return "FrameEnded";
        case volt::event::EventType::kWindowResized:
            return "WindowResized";
        case volt::event::EventType::kWindowMinimized:
            return "WindowMinimized";
        case volt::event::EventType::kKeyInput:
            return "KeyInput";
        case volt::event::EventType::kMouseMoved:
            return "MouseMoved";
        case volt::event::EventType::kMouseButton:
            return "MouseButton";
        case volt::event::EventType::kMouseScrolled:
            return "MouseScrolled";
        case volt::event::EventType::kRenderFrameBegin:
            return "RenderFrameBegin";
        case volt::event::EventType::kRenderScenePassBegin:
            return "RenderScenePassBegin";
        case volt::event::EventType::kRenderScenePassEnd:
            return "RenderScenePassEnd";
        case volt::event::EventType::kRenderUiPass:
            return "RenderUiPass";
        case volt::event::EventType::kRenderFrameEnd:
            return "RenderFrameEnd";
        case volt::event::EventType::kUiFrameBegin:
            return "UiFrameBegin";
        case volt::event::EventType::kUiLayoutPass:
            return "UiLayoutPass";
        case volt::event::EventType::kUiPaintPass:
            return "UiPaintPass";
        case volt::event::EventType::kUiFrameEnd:
            return "UiFrameEnd";
        case volt::event::EventType::kImportStarted:
            return "ImportStarted";
        case volt::event::EventType::kImportStageComplete:
            return "ImportStageComplete";
        case volt::event::EventType::kImportSucceeded:
            return "ImportSucceeded";
        case volt::event::EventType::kImportFailed:
            return "ImportFailed";
        case volt::event::EventType::kUnknown:
        default:
            return "Unknown";
        }
    }

    [[maybe_unused]] inline bool createCodecRenderSmokeAssets()
    {
        constexpr std::uint32_t kWidth = 128;
        constexpr std::uint32_t kHeight = 128;

        volt::io::RawImage image{};
        image.width = kWidth;
        image.height = kHeight;
        image.rgba.resize(static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) * 4U);

        for (std::uint32_t y = 0; y < kHeight; ++y)
        {
            for (std::uint32_t x = 0; x < kWidth; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(kWidth) + x) * 4U;
                const bool checker = ((x / 16U) + (y / 16U)) % 2U == 0U;
                image.rgba[i + 0U] = checker ? 250U : 32U;
                image.rgba[i + 1U] = static_cast<std::uint8_t>((x * 255U) / kWidth);
                image.rgba[i + 2U] = static_cast<std::uint8_t>((y * 255U) / kHeight);
                image.rgba[i + 3U] = 255U;
            }
        }

        const std::filesystem::path dir = std::filesystem::path("assets") / "images";
        const std::filesystem::path png = dir / "codec-render-test.png";
        const std::filesystem::path jpg = dir / "codec-render-test.jpg";
        const std::filesystem::path bmp = dir / "codec-render-test.bmp";

        const bool wrotePng = volt::io::encodeImageFile(png, image, volt::io::ImageEncodeFormat::kPng);
        const bool wroteJpg = volt::io::encodeImageFile(jpg, image, volt::io::ImageEncodeFormat::kJpeg, 92);
        const bool wroteBmp = volt::io::encodeImageFile(bmp, image, volt::io::ImageEncodeFormat::kBmp);

        volt::io::RawImage decoded{};
        const bool decodedPng = wrotePng && volt::io::decodeImageFile(png, decoded);
        const bool decodedJpg = wroteJpg && volt::io::decodeImageFile(jpg, decoded);
        const bool decodedBmp = wroteBmp && volt::io::decodeImageFile(bmp, decoded);

        const char *jpgStatus = wroteJpg ? (decodedJpg ? "ok" : "fail") : "unsupported";

        VOLT_LOG_INFO_CAT(
            volt::core::logging::Category::kIO,
            "Image codec smoke test: png=",
            decodedPng ? "ok" : "fail",
            " jpg=",
            jpgStatus,
            " bmp=",
            decodedBmp ? "ok" : "fail");

        return decodedPng && decodedBmp && (!wroteJpg || decodedJpg);
    }
}