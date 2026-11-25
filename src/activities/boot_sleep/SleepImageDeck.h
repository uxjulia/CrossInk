#pragma once

#include <cstdint>

namespace SleepImageDeck {

enum class Result { Selected, NoEligible, Error };

// Pick from the remaining cards, optionally restricted by an eligibility bitmap.
// Record the render attempt before requesting another pick from this deck.
Result select(const char* indexPath, uint64_t fingerprint, uint16_t count, uint16_t& index,
              const uint8_t* eligible = nullptr);
bool recordAttempt(const char* indexPath, uint64_t fingerprint, uint16_t count, uint16_t index, bool displayed);
// Skip known-invalid BMPs for the round, keeping temporary PNG filtering separate.
bool skipInvalid(const char* indexPath, uint64_t fingerprint, uint16_t count, const uint8_t* valid);

}  // namespace SleepImageDeck
