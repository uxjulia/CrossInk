#pragma once

// Caller-owned observations for one synchronous stream request. No URLs or
// credentials are retained. Each redirect hop replaces the previous response.
struct HttpRequestDiagnostics {
  int status = 0;
  bool attempted = false;
  bool basicChallenge = false;
  bool authorizationOriginMatched = false;
  bool challengeComplete = false;
  bool authorizationWithheld = false;
  bool callbackRejected = false;
};
