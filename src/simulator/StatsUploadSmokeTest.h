#pragma once
#ifdef SIMULATOR
class GfxRenderer;
class MappedInputManager;
void verifySyncDiagnosticsScreens(GfxRenderer& renderer, MappedInputManager& input);
// Runs only under the explicit simulator smoke-test flag, with isolated storage.
void verifyStatsUploadContract();
#endif
