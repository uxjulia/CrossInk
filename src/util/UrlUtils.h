#pragma once
#include <string>

namespace UrlUtils {

// Omit query tokens, fragments, and credentials from diagnostic URLs.
std::string forLog(const std::string& url);

/**
 * Check if URL uses HTTPS protocol
 */
bool isHttpsUrl(const std::string& url);

/**
 * Prepend http:// if no protocol specified (server will redirect to https if needed)
 */
std::string ensureProtocol(const std::string& url);

/**
 * Extract host with protocol from URL (e.g., "http://example.com" from "http://example.com/path")
 */
std::string extractHost(const std::string& url);

/**
 * Percent-encode raw characters that esp_http_client rejects in a URL.
 */
std::string encodeUnsafeUrlChars(const std::string& url);

/**
 * Build full URL from server URL and path.
 * If path starts with /, it's an absolute path from the host root.
 * Otherwise, it's relative to the server URL.
 */
std::string buildUrl(const std::string& serverUrl, const std::string& path);

// Resolve an HTTP document link (including query-only and parent references).
// Unlike buildUrl's directory join, a relative link replaces the base filename.
std::string resolveReference(const std::string& baseUrl, const std::string& reference);

}  // namespace UrlUtils
