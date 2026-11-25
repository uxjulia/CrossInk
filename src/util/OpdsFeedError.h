#pragma once

#include "network/HttpRequestDiagnostics.h"

// Adapted from Marco Rojas, CrossPoint #3872 (26ccbe2816dd8a9b2b4f5c89ffaac6f1afbdaedd).
enum class OpdsFeedError { Other, Unauthorized, Redirected, ServerError, Unreachable };

// Only classify failed HTTP transfers; cancellation, parser and local failures
// stay generic. A received 200 with an incomplete body is not "unreachable".
constexpr OpdsFeedError classifyFeedFailure(const HttpRequestDiagnostics& request, bool httpFailure) {
  if (!httpFailure || request.callbackRejected || !request.attempted) return OpdsFeedError::Other;
  if (request.status <= 0) return OpdsFeedError::Unreachable;
  if (request.status == 401)
    return request.authorizationWithheld ? OpdsFeedError::Redirected : OpdsFeedError::Unauthorized;
  if (request.status >= 500 && request.status < 600) return OpdsFeedError::ServerError;
  return OpdsFeedError::Other;
}
