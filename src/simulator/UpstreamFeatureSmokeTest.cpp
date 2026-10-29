#ifdef SIMULATOR
#include "UpstreamFeatureSmokeTest.h"

#include <Bitmap.h>
#include <Epub.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/boot_sleep/SleepActivity.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

struct UpstreamFeatureSmokeTest {
  static void require(bool value, const char* message) {
    if (value) return;
    LOG_ERR("PORTTEST", "%s", message);
    std::_Exit(2);
  }
  static void capture(const char* name) {
    const char* output = std::getenv("CROSSINK_UPSTREAM_FEATURE_CAPTURES");
    if (!output) return;
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output) / (std::string(name) + ".pgm");
    FILE* image = std::fopen(path.c_str(), "wb");
    require(image != nullptr, "capture open");
    std::fprintf(image, "P5\n%d %d\n255\n", renderer.getScreenWidth(), renderer.getScreenHeight());
    for (int y = 0; y < renderer.getScreenHeight(); ++y)
      for (int x = 0; x < renderer.getScreenWidth(); ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
    require(std::fclose(image) == 0, "capture close");
  }
  static void run() {
    RenderLock lock;
    require(CrossPointSettings::verifySleepScreenMigrationContract(), "sleep settings migration");
    Epub book("/port.epub", "/.crosspoint");
    require(book.load(), "GIF EPUB load");
    require(book.generateCoverBmp(false, &renderer, 0, false), "GIF cover conversion");
    FsFile invalidThumbnail;
    require(Storage.openFileForWrite("PORTTEST", book.getThumbBmpPath(120, 160), invalidThumbnail),
            "invalid thumbnail fixture");
    invalidThumbnail.print("invalid cached BMP");
    invalidThumbnail.close();
    require(book.generateThumbBmp(120, 160, &renderer), "GIF cropped thumbnail");
    require(book.generateAdaptiveThumbBmp(120, 160, &renderer), "GIF adaptive thumbnail");
    FsFile thumbnail;
    require(Storage.openFileForRead("PORTTEST", book.getThumbBmpPath(120, 160), thumbnail), "thumbnail open");
    Bitmap thumb(thumbnail);
    require(thumb.parseHeaders() == BmpReaderError::Ok && thumb.getWidth() == 120 && thumb.getHeight() == 160,
            "thumbnail dimensions");
    thumbnail.close();
    require(ImageDecoderFactory::isFormatSupported("/cover.GIF"), "GIF decoder admission");
    auto* decoder = ImageDecoderFactory::getDecoder("/cover.gif");
    require(decoder != nullptr, "GIF decoder creation");
    RenderConfig config{0, 0, 80, 80};
    config.useGrayscale = false;
    renderer.clearScreen();
    require(decoder->decodeToFramebuffer("/cover.gif", renderer, config), "inline GIF decode");
    require(!renderer.isPixelBlack(10, 40) && renderer.isPixelBlack(60, 40), "inline GIF pixel polarity");
    renderer.clearScreen();
    require(decoder->decodeToFramebuffer("/midtone.gif", renderer, config), "midtone GIF decode");
    int blackPixels = 0;
    for (int y = 0; y < 80; ++y)
      for (int x = 0; x < 80; ++x) blackPixels += renderer.isPixelBlack(x, y);
    require(blackPixels > 1600 && blackPixels < 4800, "grayscale-off GIF lost midtones");
    capture("gif-grayscale-off");
    const auto originalOrientation = renderer.getOrientation();
    config.x = config.y = -10;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      renderer.clearScreen();
      require(decoder->decodeToFramebuffer("/cover.gif", renderer, config), "clipped GIF decode");
      require(!renderer.isPixelBlack(10, 30) && renderer.isPixelBlack(50, 30), "clipped GIF pixel position");
      require(!renderer.isPixelBlack(75, 30) && !renderer.isPixelBlack(50, 75), "GIF wrote outside image bounds");
    }
    renderer.setOrientation(originalOrientation);
    SETTINGS.sleepScreen = CrossPointSettings::COVER_OVERLAY;
    SETTINGS.sleepScreenCoverMode = CrossPointSettings::FIT;
    SETTINGS.sleepScreenCoverFilter = CrossPointSettings::BLACK_AND_WHITE;
    SleepActivity sleep(renderer, mappedInputManager, false, "/port.epub");
    const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
    // Force only the renderer's absolute-plane state: native display capabilities
    // are deliberately false. This verifies session lifetime and mask pixels,
    // not panel waveform behavior.
    FsFile overlayFile, coverFile;
    require(Storage.openFileForRead("PORTTEST", "/overlay.bmp", overlayFile), "gray mask fixture open");
    require(Storage.openFileForRead("PORTTEST", book.getCoverBmpPath(false, false), coverFile), "gray cover open");
    Bitmap overlay(overlayFile), cover(coverFile);
    require(overlay.parseHeaders() == BmpReaderError::Ok && cover.parseHeaders() == BmpReaderError::Ok,
            "gray bitmap headers");
    const auto placement = sleep.prepareSleepBitmap(cover);
    renderer.absoluteGrayPlanes = true;
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode);
      require(sleep.drawSleepBitmap(cover, placement), "filtered cover plane");
      require(renderer.grayPlanesAreAbsolute() && renderer.getRenderMode() == mode,
              "filtered cover cancelled grayscale session");
      require(overlay.rewindToData() == BmpReaderError::Ok, "gray mask rewind");
      require(renderer.drawBitmap(overlay, 0, 0, width, height, 0, 0, true), "gray mask draw");
      require(renderer.grayPlanesAreAbsolute() && renderer.getRenderMode() == mode,
              "BMP mask cancelled grayscale session");
      require(renderer.isPixelBlack(width / 4, height / 2), "midtone overlay was not a black mask");
      require(renderer.isPixelBlack(width * 3 / 4, height / 2) && !renderer.isPixelBlack(width / 4, height / 2 - 30),
              "mask transparency changed the cover");
    }
    renderer.setRenderMode(GfxRenderer::BW);
    overlayFile.close();
    coverFile.close();
    for (const auto* path : {"/overlay.bmp", "/overlay.png"}) {
      APP_STATE.favoriteSleepImagePath = path;
      sleep.renderCoverSleepScreen();
      capture(path + 1);
      require(renderer.isPixelBlack(width / 4, height / 2), "overlay mask missing on cover");
      require(renderer.isPixelBlack(width * 3 / 4, height / 2), "transparent overlay erased cover");
      require(!renderer.isPixelBlack(width / 4, height / 2 - 30), "cover white region corrupted");
    }
    APP_STATE.favoriteSleepImagePath.clear();
    sleep.renderCoverSleepScreen();
    require(!renderer.isPixelBlack(width / 4, height / 2), "missing overlay did not preserve plain cover");
    LOG_INF("PORTTEST", "GIF covers, thumbnails, inline pixels, cover overlays and settings migration passed");
    std::_Exit(0);
  }
};
bool tickUpstreamFeatureSmokeTest() {
  if (!std::getenv("CROSSINK_SIMULATOR_SMOKE_UPSTREAM_FEATURES")) return false;
  UpstreamFeatureSmokeTest::run();
  return true;
}
#endif
