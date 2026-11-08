#pragma once
#if defined(SIMULATOR) && CROSSINK_APP_CAP_TOUCH
class EpubReaderActivity;
struct ClippingSelectionSmokeTest {
  static bool tick();
  static void verifyStableHighlightAnchoring(EpubReaderActivity& reader);
  static void verifyHighlightAnchoring(EpubReaderActivity& reader);
  static void verifySavedHighlightSpacing(EpubReaderActivity& reader);
};
#endif
