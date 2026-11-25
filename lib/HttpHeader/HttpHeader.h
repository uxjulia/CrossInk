#pragma once
#include <string>
#include <string_view>
struct HttpHeader {
  std::string name;
  std::string value;
};
bool isValidHttpHeader(std::string_view name, std::string_view value);
bool parseHttpHeaderLine(std::string_view line, HttpHeader& header);
std::string formatHttpHeaderLine(const HttpHeader& header, bool maskValue = false);
// Both simulator and device transports accept null-terminated header strings.
template <typename Client, typename Headers>
void applyCustomHttpHeaders(Client& client, const Headers& headers) {
  for (const auto& header : headers) {
    if (!header.name.empty()) client.addHeader(header.name.c_str(), header.value.c_str());
  }
}
