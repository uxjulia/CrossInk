#include <HyphenationPack.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace hyphenation_pack;
namespace {
constexpr size_t PARTITION_SIZE = 0x360000;
constexpr size_t UI_BYTES = 2 * 65536;
constexpr size_t CAPACITY = PARTITION_SIZE - UI_BYTES;
void put32(uint8_t* p, uint32_t n) {
  for (size_t i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(n >> (8 * i));
}
bool supported(const char* code, uint8_t prefix, uint8_t suffix) {
  const char* codes[] = {"de", "fr", "ru", "uk", "es", "it", "pt", "sv", "pl"};
  for (const auto* candidate : codes)
    if (!std::strcmp(candidate, code)) return prefix == 2 && suffix == 2;
  return false;
}
struct Device {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(PARTITION_SIZE, 0xff);
  int failAt = -1;
  int failReadOffset = -1;
  int mutations = 0;
  bool corruptWrite = false;
  Device() { std::fill(bytes.begin() + CAPACITY, bytes.end(), 0xa5); }
  bool fail() { return mutations++ == failAt; }
  Flash flash() {
    return {this,
            CAPACITY,
            [](void* p, size_t off, void* out, size_t size) {
              auto& device = *static_cast<Device*>(p);
              assert(off <= CAPACITY && size <= CAPACITY - off);
              if (device.failReadOffset >= 0 && off == static_cast<size_t>(device.failReadOffset)) return false;
              std::memcpy(out, device.bytes.data() + off, size);
              return true;
            },
            [](void* p, size_t off, const void* in, size_t size) {
              auto& device = *static_cast<Device*>(p);
              assert(off <= CAPACITY && size <= CAPACITY - off);
              if (device.fail()) return false;
              const auto* data = static_cast<const uint8_t*>(in);
              for (size_t i = 0; i < size; ++i) {
                assert((device.bytes[off + i] & data[i]) == data[i]);
                device.bytes[off + i] = data[i];
              }
              if (device.corruptWrite && size) device.bytes[off] ^= 1;
              return true;
            },
            [](void* p, size_t off, size_t size) {
              auto& device = *static_cast<Device*>(p);
              assert(off % SECTOR_SIZE == 0 && size % SECTOR_SIZE == 0);
              assert(off <= CAPACITY && size <= CAPACITY - off);
              if (device.fail()) return false;
              std::fill_n(device.bytes.begin() + off, size, 0xff);
              return true;
            },
            nullptr};
  }
  void checkUi() const {
    assert(std::all_of(bytes.begin() + CAPACITY, bytes.end(), [](auto b) { return b == 0xa5; }));
  }
};
Input input(std::vector<uint8_t>& bytes) {
  return {&bytes, bytes.size(), [](void* p, size_t off, void* out, size_t size) {
            auto& data = *static_cast<std::vector<uint8_t>*>(p);
            if (off > data.size() || size > data.size() - off) return false;
            std::memcpy(out, data.data() + off, size);
            return true;
          }};
}
std::vector<uint8_t> pack(const char* code, size_t size = 2301, uint8_t content = 0x42) {
  std::vector<uint8_t> data(PACK_HEADER_SIZE + size, content);
  std::fill_n(data.begin(), PACK_HEADER_SIZE, 0);
  std::memcpy(data.data(), "CPHY\1", 5);
  std::memcpy(data.data() + 5, code, 2);
  data[7] = data[8] = 2;
  put32(data.data() + 12, size - 1);
  put32(data.data() + 16, size);
  put32(data.data() + 20, crc32(data.data() + PACK_HEADER_SIZE, size));
  return data;
}
bool locallySupported(const char* code, uint8_t prefix, uint8_t suffix) {
  return prefix == 2 && suffix == 2 &&
         (!std::strcmp(code, "cs") || !std::strcmp(code, "fi") || !std::strcmp(code, "tr"));
}

Store boot(Device& device) {
  Store store;
  assert(store.begin(device.flash(), supported) == Result::Ok);
  return store;
}

Store bootWithLocal(Device& device) {
  Store store;
  assert(store.begin(device.flash(), supported, locallySupported) == Result::Ok);
  return store;
}

std::vector<uint8_t> validLocalPack(const char* code, uint8_t edge = 'a') {
  auto source = pack(code, 4);
  // Empty levels tape, leaf at zero, root at one with a backward edge.
  source[24] = 0;
  source[25] = 1;
  source[26] = edge;
  source[27] = 255;
  put32(source.data() + 12, 1);
  put32(source.data() + 20, crc32(source.data() + PACK_HEADER_SIZE, 4));
  return source;
}
void sourceValidation() {
  // Empty levels tape; leaf node at zero, root at one with one backward edge.
  auto source = pack("de", 4);
  source[24] = 0;
  source[25] = 1;
  source[26] = 'a';
  source[27] = 255;
  put32(source.data() + 12, 1);
  const auto checksum = [](auto& bytes) {
    put32(bytes.data() + 20, crc32(bytes.data() + PACK_HEADER_SIZE, bytes.size() - PACK_HEADER_SIZE));
  };
  checksum(source);
  Entry entry;
  assert(validate(input(source), supported, entry) == Result::Ok);
  assert(!std::strcmp(entry.code, "de") && entry.rootOffset == 1);
  ValidationOptions options;
  options.expectedCode = "fr";
  assert(validate(input(source), supported, entry, options) == Result::Invalid);
  options = {};
  options.maxPayloadSize = 3;
  assert(validate(input(source), supported, entry, options) == Result::NoSpace);
  options = {};
  options.continueWork = [](void*) { return false; };
  assert(validate(input(source), supported, entry, options) == Result::Cancelled);
  auto bad = source;
  bad[27] = 0;  // self-cycle, even with a correct CRC
  checksum(bad);
  assert(validate(input(bad), supported, entry) == Result::Invalid);
  bad = source;
  bad[27] = 1;  // forward edge into its own transition byte
  checksum(bad);
  assert(validate(input(bad), supported, entry) == Result::Invalid);
  bad = source;
  bad[24] = 128;  // levels pointer cannot fit before the parent
  checksum(bad);
  assert(validate(input(bad), supported, entry) == Result::Invalid);
  bad = source;
  bad[26] ^= 1;  // CRC mismatch
  assert(validate(input(bad), supported, entry) == Result::Invalid);
  for (const size_t offset : {size_t{4}, size_t{9}, size_t{10}, size_t{11}, size_t{12}, size_t{16}}) {
    bad = source;
    bad[offset] ^= 0x80;
    assert(validate(input(bad), supported, entry) != Result::Ok);
  }
  bad = source;
  bad.push_back(0);
  assert(validate(input(bad), supported, entry) == Result::Invalid);
  bad = pack("de", MAX_SOURCE_SIZE);
  assert(validate(input(bad), supported, entry) == Result::NoSpace);
  Input broken = input(source);
  broken.read = [](void*, size_t, void*, size_t) { return false; };
  assert(validate(broken, supported, entry) == Result::Io);
  assert(validate({}, supported, entry) == Result::Invalid);
}
void storageTests() {
  assert(crc32("123456789", 9) == 0xcbf43926u);
  assert(crc32("56789", 5, crc32("1234", 4)) == 0xcbf43926u);
  Device device;
  auto store = boot(device);
  assert(store.count() == 0);
  assert(store.availablePayloadBytes("de") == CAPACITY / 2 - SECTOR_SIZE);
  auto german = pack("de");
  assert(store.install(input(german)) == Result::Ok);
  assert(store.needsRestart());
  assert(store.availablePayloadBytes("de") == 0);
  assert(store.count() == 0);  // publication never mutates the pinned bank
  assert(store.install(input(german)) == Result::RestartRequired);
  store = boot(device);
  Entry first;
  assert(store.find("de", first));
  assert(store.availablePayloadBytes("de") == CAPACITY / 2 - SECTOR_SIZE);
  assert(store.availablePayloadBytes("fr") == CAPACITY / 2 - SECTOR_SIZE - 2304);
  const auto identityBefore = identity(first);
  device.mutations = 0;
  assert(store.install(input(german)) == Result::Unchanged);
  assert(device.mutations == 0);
  auto french = pack("fr", 1073, 0x11);
  assert(store.install(input(french)) == Result::Ok);
  assert(store.count() == 1);
  store = boot(device);
  assert(store.count() == 2);
  Entry relocated;
  assert(store.find("de", relocated));
  assert(identity(relocated) == identityBefore);
  device.checkUi();

  // Each possible interrupted erase/write must leave the previous bank usable.
  const Device baseline = device;
  Device retry = baseline;
  Store retrying;
  retry.failReadOffset = CAPACITY / 2;
  assert(retrying.begin(retry.flash(), supported) == Result::Io);
  retry.failReadOffset = -1;
  retry.bytes[0] ^= 1;
  retry.bytes[CAPACITY / 2] ^= 1;
  assert(retrying.begin(retry.flash(), supported) == Result::Ok);
  assert(retrying.count() == 0);  // No bank retained from the failed scan.
  auto update = pack("de", 3101, 0x28);
  Device successful = baseline;
  successful.mutations = 0;
  auto updateStore = boot(successful);
  assert(updateStore.install(input(update)) == Result::Ok);
  const int operations = successful.mutations;
  for (int stop = 0; stop < operations; ++stop) {
    Device interrupted = baseline;
    interrupted.mutations = 0;
    interrupted.failAt = stop;
    auto old = boot(interrupted);
    assert(old.install(input(update)) == Result::Io);
    interrupted.failAt = -1;
    auto recovered = boot(interrupted);
    Entry entry;
    assert(recovered.count() == 2 && recovered.find("de", entry));
    assert(identity(entry) == identityBefore);
    interrupted.checkUi();
  }
  // Silent write corruption is caught by readback before a commit is published.
  Device broken = baseline;
  broken.corruptWrite = true;
  auto old = boot(broken);
  assert(old.install(input(update)) == Result::Io);
  assert(boot(broken).find("de", first) && identity(first) == identityBefore);

  auto newest = boot(successful);
  assert(newest.find("de", first));
  assert(identity(first) != identityBefore);
  successful.bytes[newest.bankOffset() + first.offset] ^= 1;
  auto fallback = boot(successful);
  assert(fallback.find("de", first) && identity(first) == identityBefore);

  assert(store.remove("de") == Result::Ok);
  store = boot(device);
  assert(store.count() == 1 && !store.find("de", first) && store.find("fr", first));
  assert(store.remove("fr") == Result::Ok);
  store = boot(device);
  assert(store.count() == 0);
  device.checkUi();

  for (const size_t offset : {size_t{0}, size_t{4}, size_t{9}, size_t{10}, size_t{16}, size_t{20}, size_t{24}}) {
    auto bad = german;
    bad[offset] ^= 0x80;
    assert(store.install(input(bad)) != Result::Ok);
    assert(store.count() == 0);
  }
  auto unsupported = pack("el");
  assert(store.install(input(unsupported)) == Result::Unsupported);
  auto english = pack("en");
  assert(store.install(input(english)) == Result::Unsupported);
  auto tooLarge = pack("de", CAPACITY / 2);
  assert(store.install(input(tooLarge)) == Result::NoSpace);
  auto truncated = german;
  truncated.pop_back();
  assert(store.install(input(truncated)) == Result::Invalid);
  auto flags = german;
  flags[7] = 3;
  assert(store.install(input(flags)) == Result::Unsupported);
  auto rootChanged = german;
  put32(rootChanged.data() + 12, 0);
  assert(store.install(input(german)) == Result::Ok);
  store = boot(device);
  assert(store.install(input(rootChanged)) == Result::Ok);  // metadata changes count even with identical payload
  device.checkUi();
  std::puts("Atomic pack storage, recovery, CRC and UI-slot isolation passed");
}

void localOnlyBankDoesNotResurrectRemovedPacks() {
  Device device;
  auto german = pack("de");
  auto local = validLocalPack("cs");
  assert(boot(device).install(input(german)) == Result::Ok);
  assert(bootWithLocal(device).install(input(local)) == Result::Ok);
  assert(bootWithLocal(device).remove("de") == Result::Ok);

  auto current = bootWithLocal(device);
  auto legacy = boot(device);
  Entry entry;
  assert(current.count() == 1 && current.find("cs", entry) && !current.find("de", entry));
  assert(legacy.bankOffset() == current.bankOffset());
  assert(legacy.count() == 0 && !legacy.find("de", entry) && !legacy.find("cs", entry));
  assert(current.remove("cs") == Result::Ok);
  assert(bootWithLocal(device).count() == 0);
  assert(boot(device).count() == 0);
  device.checkUi();
}

void localTailReservationTests() {
  Device device;
  const size_t reserve = CAPACITY / 2 - 2 * SECTOR_SIZE;
  auto bootReserved = [&] {
    Store store;
    assert(store.begin(device.flash(), supported, locallySupported, reserve) == Result::Ok);
    return store;
  };
  auto german = pack("de", SECTOR_SIZE - 4);
  auto french = pack("fr", 16);
  auto czech = validLocalPack("cs");
  assert(bootReserved().install(input(german)) == Result::Ok);
  auto store = bootReserved();
  assert(store.availablePayloadBytes("cs") == 4);
  assert(store.install(input(czech)) == Result::Ok);
  store = bootReserved();
  assert(store.usedBytes() == 2 * SECTOR_SIZE && store.availablePayloadBytes("fr") == 0);
  device.mutations = 0;
  assert(store.install(input(french)) == Result::NoSpace);
  assert(device.mutations == 0);
  assert(store.remove("cs") == Result::Ok);
  // No extension: retain legacy ability to install into the remaining bank.
  assert(bootReserved().install(input(french)) == Result::Ok);
  assert(boot(device).count() == 2);
  device.checkUi();
}

void localDiscoveryStorageTests() {
  Device device;
  auto german = pack("de");
  auto czech = validLocalPack("cs");
  auto finnish = validLocalPack("fi", 'f');
  auto turkish = validLocalPack("tr", 't');
  assert(boot(device).install(input(german)) == Result::Ok);

  auto store = bootWithLocal(device);
  Entry entry;
  assert(store.find("de", entry));
  const uint32_t germanId = identity(entry);
  assert(store.install(input(czech)) == Result::Ok);
  assert(store.count() == 1 && !store.find("cs", entry));
  store = bootWithLocal(device);
  assert(store.count() == 2 && store.find("cs", entry));
  const uint32_t czechId = identity(entry);
  const size_t firstDynamicBank = store.bankOffset();
  assert(store.install(input(finnish)) == Result::Ok);
  store = bootWithLocal(device);
  assert(store.bankOffset() != firstDynamicBank);
  assert(store.count() == 3 && store.find("fi", entry));
  std::vector<std::string> codes;
  for (size_t index = 0; index < store.count(); ++index) {
    assert(store.entryAt(index, entry));
    codes.emplace_back(entry.code);
  }
  std::sort(codes.begin(), codes.end());
  assert((codes == std::vector<std::string>{"cs", "de", "fi"}));
  assert(!store.entryAt(store.count(), entry));

  // Both banks now contain extensions: an old reader must see the same
  // newest known payload, not fall back to a prehistoric known-only bank.
  auto legacy = boot(device);
  assert(legacy.bankOffset() == store.bankOffset());
  assert(legacy.count() == 1 && legacy.find("de", entry) && identity(entry) == germanId);
  assert(!legacy.find("cs", entry) && !legacy.find("fi", entry));

  auto bad = turkish;
  bad[27] = 0;
  put32(bad.data() + 20, crc32(bad.data() + PACK_HEADER_SIZE, 4));
  device.mutations = 0;
  assert(store.install(input(bad)) == Result::Invalid);
  assert(device.mutations == 0);

  // Exercise both namespaces in the same rewrite. Changes remain invisible
  // until a restart, and removing a local item cannot lose other content.
  auto updatedGerman = pack("de", 2313, 0x53);
  assert(store.install(input(updatedGerman)) == Result::Ok);
  store = bootWithLocal(device);
  assert(store.find("de", entry) && identity(entry) != germanId);
  const uint32_t updatedGermanId = identity(entry);
  assert(store.find("cs", entry) && identity(entry) == czechId);
  assert(store.find("fi", entry));
  assert(store.remove("cs") == Result::Ok);
  store = bootWithLocal(device);
  assert(store.count() == 2 && !store.find("cs", entry) && store.find("fi", entry));
  assert(store.find("de", entry) && identity(entry) == updatedGermanId);
  device.checkUi();

  // Interrupted rewrites cannot expose a new local suffix without the
  // original commit marker or lose the previously pinned known/local data.
  Device complete = device;
  complete.mutations = 0;
  assert(bootWithLocal(complete).install(input(turkish)) == Result::Ok);
  assert(complete.mutations > 0);
  for (int stop = 0; stop < complete.mutations; ++stop) {
    Device interrupted = device;
    interrupted.mutations = 0;
    interrupted.failAt = stop;
    assert(bootWithLocal(interrupted).install(input(turkish)) == Result::Io);
    interrupted.failAt = -1;
    auto recovered = bootWithLocal(interrupted);
    assert(recovered.count() == 2 && !recovered.find("tr", entry) && recovered.find("fi", entry));
    assert(recovered.find("de", entry) && identity(entry) == updatedGermanId);
    assert(boot(interrupted).find("de", entry) && identity(entry) == updatedGermanId);
    interrupted.checkUi();
  }

  // Damage only the committed extension (once in its header, and once in
  // its payload). Existing firmware must still open the newest known data.
  // Current firmware must not return any data from the damaged suffix.
  auto newest = bootWithLocal(complete);
  assert(newest.find("tr", entry));
  const size_t bank = newest.bankOffset();
  for (const auto offset : {bank + HEADER_SIZE, bank + entry.offset}) {
    Device corrupted = complete;
    corrupted.bytes[offset] ^= 1;
    auto oldReader = boot(corrupted);
    assert(oldReader.bankOffset() == bank && oldReader.find("de", entry) && identity(entry) == updatedGermanId);
    auto newReader = bootWithLocal(corrupted);
    assert(newReader.find("de", entry) && identity(entry) == updatedGermanId);
    assert(!newReader.find("tr", entry));
    corrupted.checkUi();
  }

  // A downlevel firmware install intentionally rewrites only what it knows.
  // Its newer known bank must still win when current firmware boots again.
  auto french = pack("fr");
  assert(boot(complete).install(input(french)) == Result::Ok);
  auto upgraded = bootWithLocal(complete);
  assert(upgraded.count() == 2 && upgraded.find("fr", entry) && upgraded.find("de", entry));
  assert(!upgraded.find("fi", entry) && !upgraded.find("tr", entry));
  complete.checkUi();
  std::puts("SD-local extension rotation, rollback, validation and interrupted writes passed");
}
}  // namespace
int main(int argc, char** argv) {
  sourceValidation();
  storageTests();
  localDiscoveryStorageTests();
  localOnlyBankDoesNotResurrectRemovedPacks();
  localTailReservationTests();
  // SD-only packs are installed individually; the reader has a 10-pack
  // bank limit, so do not assume every pack in the release ZIP fits at once.
  if (argc > 1 && std::strcmp(argv[1], "--sd-packs") == 0) {
    for (int i = 2; i < argc; ++i) {
      std::ifstream stream(argv[i], std::ios::binary);
      assert(stream);
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
      Entry entry;
      const auto local = [](const char* code, uint8_t prefix, uint8_t suffix) {
        return code && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' && code[1] <= 'z' && !code[2] &&
               std::strcmp(code, "en") && prefix == 2 && suffix == 2;
      };
      const auto valid = validate(input(bytes), local, entry);
      if (valid != Result::Ok) std::fprintf(stderr, "%s: %s\n", argv[i], resultName(valid));
      assert(valid == Result::Ok);
      assert(!supported(entry.code, entry.minPrefix, entry.minSuffix));
      char code[3];
      std::memcpy(code, entry.code, sizeof(code));
      const uint32_t packId = identity(entry);
      Device flash;
      Store store;
      constexpr size_t C3_RESERVED_TAIL = 384 * 1024;
      assert(store.begin(flash.flash(), supported, local, C3_RESERVED_TAIL) == Result::Ok);
      assert(store.install(input(bytes)) == Result::Ok);
      Store restarted;
      assert(restarted.begin(flash.flash(), supported, local, C3_RESERVED_TAIL) == Result::Ok);
      assert(restarted.count() == 1 && restarted.find(code, entry) && identity(entry) == packId);
      assert(boot(flash).count() == 0);
      flash.checkUi();
    }
    return 0;
  }

  // Exercise release-generated real dictionaries through the production loader.
  Device device;
  for (int i = 1; i < argc; ++i) {
    std::ifstream stream(argv[i], std::ios::binary);
    assert(stream);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    Entry checked;
    size_t polls = 0;
    ValidationOptions options;
    options.context = &polls;
    options.continueWork = [](void* context) {
      ++*static_cast<size_t*>(context);
      return true;
    };
    assert(validate(input(bytes), supported, checked, options) == Result::Ok);
    // A yielding callback must not run once per trie node/edge. This bound
    // catches the previous hundreds of thousands of calls for the German pack.
    assert(polls <= bytes.size() / 16 + 64);
    polls = 0;
    options.continueWork = [](void* context) { return ++*static_cast<size_t*>(context) < 3; };
    assert(validate(input(bytes), supported, checked, options) == Result::Cancelled);
    assert(polls == 3 && !checked.code[0]);
    auto store = boot(device);
    assert(store.install(input(bytes)) == Result::Ok);
    device.checkUi();
  }
  if (argc > 1) assert(boot(device).count() == static_cast<size_t>(argc - 1));
}
