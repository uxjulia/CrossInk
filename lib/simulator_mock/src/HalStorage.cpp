#include "HalStorage.h"
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
#include <cstdio>

HalStorage HalStorage::instance;
HalStorage::HalStorage() {}
bool HalStorage::begin() { return true; }
bool HalStorage::ready() const { return true; }

class HalFile::Impl {
public:
    int fd = -1;
    std::string path;
    DIR* dir = nullptr;

    bool open(const char* p, int flags) {
        path = p;
        // The simulator's FsApiConstants.h just includes <fcntl.h> and typedef int oflag_t,
        // so all O_* constants are already native POSIX values — pass them straight through.
        fd = ::open(path.c_str(), flags, 0666);
        if (fd < 0) {
            fprintf(stderr, "[SIM] open failed: %s (flags=0x%x errno=%d %s)\n",
                    path.c_str(), flags, errno, strerror(errno));
        }
        return fd >= 0;
    }

    bool openAsDir(const char* p) {
        path = p;
        dir = opendir(p);
        return dir != nullptr;
    }

    bool isDir() const { return dir != nullptr; }
    bool isOpen() const { return fd >= 0 || dir != nullptr; }
};

HalFile::HalFile() : impl(new Impl()) {}
HalFile::~HalFile() {
    if (impl && impl->fd >= 0) { ::close(impl->fd); impl->fd = -1; }
}
HalFile::HalFile(HalFile&& other) : impl(std::move(other.impl)) {}
HalFile& HalFile::operator=(HalFile&& other) {
    if (this != &other) {
        if (impl && impl->fd >= 0) { ::close(impl->fd); impl->fd = -1; }
        impl = std::move(other.impl);
    }
    return *this;
}

void HalFile::flush() { if(impl && impl->fd >= 0) fsync(impl->fd); }
size_t HalFile::getName(char* name, size_t len) {
    if (!impl || impl->path.empty()) return 0;
    size_t slash = impl->path.rfind('/');
    std::string fname = (slash == std::string::npos) ? impl->path : impl->path.substr(slash + 1);
    size_t n = std::min(fname.size(), len - 1);
    memcpy(name, fname.c_str(), n);
    name[n] = '\0';
    return n;
}
size_t HalFile::size() {
    if (!impl || impl->fd < 0) return 0;
    off_t cur = lseek(impl->fd, 0, SEEK_CUR);
    off_t end = lseek(impl->fd, 0, SEEK_END);
    lseek(impl->fd, cur, SEEK_SET);
    return end < 0 ? 0 : (size_t)end;
}
size_t HalFile::fileSize() { return size(); }
bool HalFile::seek(size_t pos) {
    if(!impl || impl->fd < 0) return false;
    return lseek(impl->fd, (off_t)pos, SEEK_SET) >= 0;
}
bool HalFile::seekCur(int64_t offset) {
    if(!impl || impl->fd < 0) return false;
    return lseek(impl->fd, (off_t)offset, SEEK_CUR) >= 0;
}
bool HalFile::seekSet(size_t offset) {
    if(!impl || impl->fd < 0) return false;
    return lseek(impl->fd, (off_t)offset, SEEK_SET) >= 0;
}
int HalFile::available() const {
    if(!impl || impl->fd < 0) return 0;
    off_t cur = lseek(impl->fd, 0, SEEK_CUR);
    off_t end = lseek(impl->fd, 0, SEEK_END);
    lseek(impl->fd, cur, SEEK_SET);
    return (int)(end - cur);
}
size_t HalFile::position() const {
    if(!impl || impl->fd < 0) return 0;
    off_t pos = lseek(impl->fd, 0, SEEK_CUR);
    return pos < 0 ? 0 : (size_t)pos;
}
int HalFile::read(void* buf, size_t count) {
    if(!impl || impl->fd < 0) return -1;
    ssize_t n = ::read(impl->fd, buf, count);
    return (int)n;
}
int HalFile::read() {
    if(!impl || impl->fd < 0) return -1;
    uint8_t c;
    return (::read(impl->fd, &c, 1) == 1) ? c : -1;
}
size_t HalFile::write(const void* buf, size_t count) {
    if(!impl || impl->fd < 0) return 0;
    ssize_t n = ::write(impl->fd, buf, count);
    return n < 0 ? 0 : (size_t)n;
}
size_t HalFile::write(uint8_t b) {
    if(!impl || impl->fd < 0) return 0;
    return (::write(impl->fd, &b, 1) == 1) ? 1 : 0;
}
bool HalFile::rename(const char* newPath) { return false; }
bool HalFile::isDirectory() const { return impl && impl->isDir(); }
void HalFile::rewindDirectory() { if (impl && impl->dir) rewinddir(impl->dir); }
bool HalFile::close() {
    if (!impl) return true;
    if (impl->dir) { closedir(impl->dir); impl->dir = nullptr; }
    if (impl->fd >= 0) { ::close(impl->fd); impl->fd = -1; }
    return true;
}
HalFile HalFile::openNextFile() {
    if (!impl || !impl->dir) return HalFile();
    while (true) {
        struct dirent* entry = readdir(impl->dir);
        if (!entry) return HalFile();
        if (entry->d_name[0] == '.') continue;  // skip . and ..

        std::string childFsPath = impl->path;
        if (childFsPath.back() != '/') childFsPath += '/';
        childFsPath += entry->d_name;

        HalFile child;
        struct stat st;
        if (stat(childFsPath.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            child.impl->openAsDir(childFsPath.c_str());
        } else {
            child.impl->open(childFsPath.c_str(), O_RDONLY);
        }
        return child;
    }
}
bool HalFile::isOpen() const {
    if (!impl) return false;
    return impl->isOpen();
}
HalFile::operator bool() const { return isOpen(); }

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
    std::string full = "./fs_" + std::string(path);
    HalFile f;
    struct stat st;
    if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        f.impl->openAsDir(full.c_str());
    } else {
        f.impl->open(full.c_str(), oflag);
    }
    return f;
}
bool HalStorage::mkdir(const char* path, const bool /*pFlag*/) {
    std::string full = "./fs_" + std::string(path);
    // Create all intermediate directories (mkdir -p semantics).
    for (size_t i = 1; i < full.size(); ++i) {
        if (full[i] == '/') {
            ::mkdir(full.substr(0, i).c_str(), 0777);  // ignore errors (may already exist)
        }
    }
    return ::mkdir(full.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::exists(const char* path) {
    std::string full = "./fs_" + std::string(path);
    struct stat buffer;
    return (stat (full.c_str(), &buffer) == 0);
}
bool HalStorage::remove(const char* path) {
    std::string full = "./fs_" + std::string(path);
    return ::remove(full.c_str()) == 0;
}
bool HalStorage::rename(const char* oldPath, const char* newPath) {
    std::string o = "./fs_" + std::string(oldPath);
    std::string n = "./fs_" + std::string(newPath);
    return ::rename(o.c_str(), n.c_str()) == 0;
}
static bool removeDirRecursive(const std::string& full) {
    DIR* d = opendir(full.c_str());
    if (!d) return ::remove(full.c_str()) == 0;  // might be a plain file
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string child = full + "/" + entry->d_name;
        struct stat st;
        if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            removeDirRecursive(child);
        } else {
            ::remove(child.c_str());
        }
    }
    closedir(d);
    return ::rmdir(full.c_str()) == 0;
}

bool HalStorage::rmdir(const char* path) {
    return removeDirRecursive("./fs_" + std::string(path));
}
bool HalStorage::removeDir(const char* path) {
    return removeDirRecursive("./fs_" + std::string(path));
}

String HalStorage::readFile(const char* path) {
    HalFile f = open(path, O_RDONLY);
    if (!f) return String("");
    size_t s = f.size();
    std::string content(s, '\0');
    f.read((void*)content.data(), s);
    return String(content);
}
bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
    HalFile f = open(path, O_RDONLY);
    if (!f) return false;
    std::vector<char> buf(chunkSize);
    int n;
    while ((n = f.read(buf.data(), chunkSize)) > 0) {
        out.write(reinterpret_cast<const uint8_t*>(buf.data()), n);
    }
    return true;
}
size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
    HalFile f = open(path, O_RDONLY);
    if (!f) return 0;
    size_t toRead = bufferSize - 1;
    if (maxBytes > 0 && maxBytes < toRead) toRead = maxBytes;
    int n = f.read(buffer, toRead);
    if (n < 0) n = 0;
    buffer[n] = '\0';
    return n;
}
bool HalStorage::writeFile(const char* path, const String& content) {
    HalFile f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) return false;
    f.write(content.c_str(), content.length());
    return true;
}
bool HalStorage::ensureDirectoryExists(const char* path) { return mkdir(path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
    file = open(path, O_RDONLY);
    return file.isOpen();
}
bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
    file = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    return file.isOpen();
}
bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
}

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
    std::vector<String> result;
    std::string full = "./fs_" + std::string(path);
    DIR* dir = opendir(full.c_str());
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr && (int)result.size() < maxFiles) {
        if (entry->d_name[0] == '.') continue;  // skip . and ..
        std::string name = std::string(path);
        if (name.back() != '/') name += '/';
        name += entry->d_name;
        result.push_back(String(name));
    }
    closedir(dir);
    return result;
}
