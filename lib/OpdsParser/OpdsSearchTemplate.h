// Adapted from FreeInk OpdsSearchTemplate; see LICENSE.FreeInk.
#pragma once
#include <string>

/**
 * Expand an OPDS search URL template with an already percent-encoded query.
 *
 * Handles both template dialects found in the wild:
 *  - OpenSearch (OPDS 1.x): "?q={searchTerms}&page={startPage?}" — simple
 *    expressions; {searchTerms} gets the query, other (optional) parameters
 *    are replaced with an empty string per the OpenSearch spec.
 *  - RFC 6570 form-style (OPDS 2.0): "search{?query}" or
 *    "{?query,title,author}" — expands to "?query=<encoded>", dropping the
 *    other variables.
 */
std::string expandOpdsSearchTemplate(const std::string& templateUrl, const std::string& encodedQuery);

/**
 * Percent-encode a value for a URL query parameter or an
 * application/x-www-form-urlencoded body (RFC 3986 unreserved set kept
 * literal, everything else %XX).
 */
std::string opdsPercentEncode(const std::string& value);
