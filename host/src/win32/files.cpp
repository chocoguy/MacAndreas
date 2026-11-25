#include "paths.hpp"
#include "process.hpp"
#include "sync.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <format>
#include <system_error>
#include <vector>

namespace ma::host::win32 {

namespace {

constexpr uint32_t kInvalidHandle = 0xffffffffu;
constexpr uint32_t kInvalidAttributes = 0xffffffffu;
constexpr uint32_t kGenericRead = 0x80000000u;
constexpr uint32_t kGenericWrite = 0x40000000u;
constexpr uint32_t kCreateNew = 1;
constexpr uint32_t kCreateAlways = 2;
constexpr uint32_t kOpenExisting = 3;
constexpr uint32_t kOpenAlways = 4;
constexpr uint32_t kTruncateExisting = 5;
constexpr uint32_t kFlagOverlapped = 0x40000000u;
constexpr uint32_t kAttributeReadonly = 0x01;
constexpr uint32_t kAttributeDirectory = 0x10;
constexpr uint32_t kAttributeArchive = 0x20;

constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorPathNotFound = 3;
constexpr uint32_t kErrorAccessDenied = 5;
constexpr uint32_t kErrorInvalidHandle = 6;
constexpr uint32_t kErrorNoMoreFiles = 18;
constexpr uint32_t kErrorHandleEof = 38;
constexpr uint32_t kErrorFileExists = 80;
constexpr uint32_t kErrorAlreadyExists = 183;
constexpr uint32_t kErrorResourceTypeNotFound = 1813;
constexpr uint32_t kErrorResourceNameNotFound = 1814;

struct FileObject final : KObject {
    int fd = -1;
    std::string guest;
    bool overlapped = false;
    ~FileObject() override {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

struct FindObject final : KObject {
    std::vector<std::filesystem::directory_entry> entries;
    size_t next = 0;
};

struct MappingObject final : KObject {
    std::shared_ptr<FileObject> file;
    uint32_t size = 0;
};

uint32_t win_error(int e) {
    switch (e) {
    case ENOENT: return kErrorFileNotFound;
    case ENOTDIR: return kErrorPathNotFound;
    case EACCES:
    case EPERM:
    case EISDIR: return kErrorAccessDenied;
    case EEXIST: return kErrorFileExists;
    default: return 31;
    }
}

uint64_t filetime_of(const timespec& ts) {
    return (static_cast<uint64_t>(ts.tv_sec) + 11644473600ull) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100;
}

void write_filetime(uint32_t addr, uint64_t ft) {
    mem::write<uint32_t>(addr, static_cast<uint32_t>(ft));
    mem::write<uint32_t>(addr + 4, static_cast<uint32_t>(ft >> 32));
}

uint32_t attributes_of(const struct stat& st) {
    uint32_t a = S_ISDIR(st.st_mode) ? kAttributeDirectory : kAttributeArchive;
    if (!(st.st_mode & S_IWUSR)) {
        a |= kAttributeReadonly;
    }
    return a;
}

std::string guest_string(uint32_t addr) { return addr ? mem::read_cstr(addr) : std::string(); }

std::string narrow(const std::u16string& w) {
    std::string out;
    for (char16_t c : w) {
        out += c < 0x100 ? static_cast<char>(c) : '?';
    }
    return out;
}

uint32_t create_file(const std::string& name, uint32_t access, uint32_t disposition, uint32_t flags) {
    auto host = host_path_for(name);
    if (!host) {
        std::fprintf(stderr, "[FILE] CreateFile(\"%s\") is outside the mapped roots\n", name.c_str());
        set_last_error(kErrorPathNotFound);
        return kInvalidHandle;
    }
    int oflags = 0;
    bool want_write = access & kGenericWrite;
    bool want_read = (access & kGenericRead) || !want_write;
    oflags = want_read && want_write ? O_RDWR : want_write ? O_WRONLY : O_RDONLY;
    struct stat st {};
    bool existed = ::stat(host->c_str(), &st) == 0;
    switch (disposition) {
    case kCreateNew: oflags |= O_CREAT | O_EXCL; break;
    case kCreateAlways: oflags |= O_CREAT | O_TRUNC; break;
    case kOpenExisting: break;
    case kOpenAlways: oflags |= O_CREAT; break;
    case kTruncateExisting: oflags |= O_TRUNC; break;
    default: break;
    }
    if (existed && S_ISDIR(st.st_mode)) {
        set_last_error(kErrorAccessDenied);
        return kInvalidHandle;
    }
    int fd = ::open(host->c_str(), oflags | O_CLOEXEC, 0644);
    if (fd < 0) {
        uint32_t err = win_error(errno);
        if (err == kErrorFileNotFound && !std::filesystem::exists(host->parent_path())) {
            err = kErrorPathNotFound;
        }
        std::fprintf(stderr, "[FILE] CreateFile(\"%s\") failed: %u\n", name.c_str(), err);
        set_last_error(err);
        return kInvalidHandle;
    }
    auto file = std::make_shared<FileObject>();
    file->fd = fd;
    file->guest = name;
    file->overlapped = flags & kFlagOverlapped;
    uint32_t handle = handle_insert(file);
    std::fprintf(stderr, "[FILE] open \"%s\" -> 0x%x\n", name.c_str(), handle);
    set_last_error((disposition == kCreateAlways || disposition == kOpenAlways) && existed ? kErrorAlreadyExists : 0);
    return handle;
}

void complete_overlapped(uint32_t overlapped, uint32_t transferred, uint32_t status) {
    mem::write<uint32_t>(overlapped + 0, status);
    mem::write<uint32_t>(overlapped + 4, transferred);
    if (uint32_t event = mem::read<uint32_t>(overlapped + 16)) {
        signal_event(event);
    }
}

void fill_find_data(uint32_t out, const std::filesystem::directory_entry& entry) {
    std::memset(mem::ptr(out), 0, 320);
    struct stat st {};
    ::stat(entry.path().c_str(), &st);
    mem::write<uint32_t>(out, attributes_of(st));
    write_filetime(out + 4, filetime_of(st.st_birthtimespec));
    write_filetime(out + 12, filetime_of(st.st_atimespec));
    write_filetime(out + 20, filetime_of(st.st_mtimespec));
    uint64_t size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
    mem::write<uint32_t>(out + 28, static_cast<uint32_t>(size >> 32));
    mem::write<uint32_t>(out + 32, static_cast<uint32_t>(size));
    std::string name = entry.path().filename().string();
    if (name.size() > 259) {
        name.resize(259);
    }
    mem::write_cstr(out + 44, name);
}

uint32_t resource_directory() {
    uint32_t base = process().image_base;
    uint32_t nt = base + mem::read<uint32_t>(base + 0x3c);
    uint32_t rva = mem::read<uint32_t>(nt + 24 + 96 + 2 * 8);
    return rva ? base + rva : 0;
}

std::u16string resource_name(uint32_t root, uint32_t offset) {
    uint32_t addr = root + (offset & 0x7fffffffu);
    uint16_t length = mem::read<uint16_t>(addr);
    std::u16string out;
    for (uint16_t i = 0; i < length; ++i) {
        out += static_cast<char16_t>(mem::read<uint16_t>(addr + 2 + 2 * i));
    }
    return out;
}

struct ResourceKey {
    bool is_id = false;
    uint32_t id = 0;
    std::string name;
};

ResourceKey resource_key(uint32_t value, bool wide) {
    ResourceKey key;
    if (value < 0x10000) {
        key.is_id = true;
        key.id = value;
        return key;
    }
    std::string s = wide ? narrow(mem::read_wstr(value)) : mem::read_cstr(value);
    if (!s.empty() && s[0] == '#') {
        key.is_id = true;
        key.id = static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 10));
        return key;
    }
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    key.name = s;
    return key;
}

uint32_t find_entry(uint32_t root, uint32_t directory, const ResourceKey* key) {
    uint16_t named = mem::read<uint16_t>(directory + 12);
    uint16_t ids = mem::read<uint16_t>(directory + 14);
    for (uint32_t i = 0; i < static_cast<uint32_t>(named) + ids; ++i) {
        uint32_t entry = directory + 16 + 8 * i;
        uint32_t name = mem::read<uint32_t>(entry);
        uint32_t data = mem::read<uint32_t>(entry + 4);
        bool match = key == nullptr;
        if (key && key->is_id && !(name & 0x80000000u)) {
            match = (name & 0xffff) == key->id;
        } else if (key && !key->is_id && (name & 0x80000000u)) {
            std::string n = narrow(resource_name(root, name));
            for (char& c : n) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            match = n == key->name;
        }
        if (match) {
            return data;
        }
    }
    return 0xffffffffu;
}

uint32_t find_resource(uint32_t module, uint32_t name, uint32_t type, bool wide) {
    uint32_t root = resource_directory();
    if (root == 0 || (module != 0 && module != process().image_base)) {
        set_last_error(kErrorResourceTypeNotFound);
        return 0;
    }
    ResourceKey type_key = resource_key(type, wide);
    ResourceKey name_key = resource_key(name, wide);
    uint32_t level1 = find_entry(root, root, &type_key);
    if (level1 == 0xffffffffu || !(level1 & 0x80000000u)) {
        set_last_error(kErrorResourceTypeNotFound);
        return 0;
    }
    uint32_t level2 = find_entry(root, root + (level1 & 0x7fffffffu), &name_key);
    if (level2 == 0xffffffffu || !(level2 & 0x80000000u)) {
        set_last_error(kErrorResourceNameNotFound);
        return 0;
    }
    uint32_t level3 = find_entry(root, root + (level2 & 0x7fffffffu), nullptr);
    if (level3 == 0xffffffffu || (level3 & 0x80000000u)) {
        set_last_error(kErrorResourceNameNotFound);
        return 0;
    }
    return root + level3;
}

struct SystemTime {
    int year, month, dow, day, hour, minute, second;
};

SystemTime read_systemtime(uint32_t addr) {
    if (addr == 0) {
        std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_r(&now, &t);
        return {t.tm_year + 1900, t.tm_mon + 1, t.tm_wday, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec};
    }
    return {mem::read<uint16_t>(addr), mem::read<uint16_t>(addr + 2), mem::read<uint16_t>(addr + 4),
            mem::read<uint16_t>(addr + 6), mem::read<uint16_t>(addr + 8), mem::read<uint16_t>(addr + 10),
            mem::read<uint16_t>(addr + 12)};
}

std::string format_picture(std::string_view picture, const SystemTime& t) {
    static constexpr const char* kDays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                            "Thursday", "Friday", "Saturday"};
    static constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                              "July",    "August",   "September", "October", "November", "December"};
    std::string out;
    for (size_t i = 0; i < picture.size();) {
        char c = picture[i];
        if (c == '\'') {
            size_t end = picture.find('\'', i + 1);
            out += picture.substr(i + 1, (end == std::string_view::npos ? picture.size() : end) - i - 1);
            i = end == std::string_view::npos ? picture.size() : end + 1;
            continue;
        }
        size_t run = 1;
        while (i + run < picture.size() && picture[i + run] == c) {
            ++run;
        }
        int hour12 = t.hour % 12 == 0 ? 12 : t.hour % 12;
        const char* day = kDays[t.dow % 7];
        const char* month = kMonths[(t.month + 11) % 12];
        switch (c) {
        case 'd':
            out += run == 1   ? std::to_string(t.day)
                   : run == 2 ? std::format("{:02}", t.day)
                   : run == 3 ? std::string(day, 3)
                              : std::string(day);
            break;
        case 'M':
            out += run == 1   ? std::to_string(t.month)
                   : run == 2 ? std::format("{:02}", t.month)
                   : run == 3 ? std::string(month, 3)
                              : std::string(month);
            break;
        case 'y':
            out += run <= 2 ? std::format("{:02}", t.year % 100) : std::to_string(t.year);
            break;
        case 'h': out += run == 1 ? std::to_string(hour12) : std::format("{:02}", hour12); break;
        case 'H': out += run == 1 ? std::to_string(t.hour) : std::format("{:02}", t.hour); break;
        case 'm': out += run == 1 ? std::to_string(t.minute) : std::format("{:02}", t.minute); break;
        case 's': out += run == 1 ? std::to_string(t.second) : std::format("{:02}", t.second); break;
        case 't': out += run == 1 ? (t.hour < 12 ? "A" : "P") : (t.hour < 12 ? "AM" : "PM"); break;
        default: out.append(run, c); break;
        }
        i += run;
    }
    return out;
}

uint32_t write_formatted(const std::string& text, uint32_t buffer, uint32_t size) {
    uint32_t need = static_cast<uint32_t>(text.size() + 1);
    if (size == 0) {
        return need;
    }
    if (size < need) {
        set_last_error(122);
        return 0;
    }
    mem::write_cstr(buffer, text);
    return need;
}

constexpr ApiDef kFiles[] = {
    {"KERNEL32.dll", "CreateFileA", 0, 7, Conv::stdcall,
     [](ApiCall& c) { c.ret(create_file(guest_string(c.arg(0)), c.arg(1), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "CreateFileW", 0, 7, Conv::stdcall,
     [](ApiCall& c) { c.ret(create_file(narrow(mem::read_wstr(c.arg(0))), c.arg(1), c.arg(4), c.arg(5))); }},
    {"KERNEL32.dll", "ReadFile", 0, 5, Conv::stdcall,
     [](ApiCall& c) {
         auto file = handle_get<FileObject>(c.arg(0));
         if (!file) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         uint32_t overlapped = c.arg(4);
         ssize_t n = 0;
         if (overlapped != 0) {
             off_t offset = static_cast<off_t>(mem::read<uint32_t>(overlapped + 8)) |
                            (static_cast<off_t>(mem::read<uint32_t>(overlapped + 12)) << 32);
             n = ::pread(file->fd, mem::ptr(c.arg(1)), c.arg(2), offset);
         } else {
             n = ::read(file->fd, mem::ptr(c.arg(1)), c.arg(2));
         }
         if (n < 0) {
             set_last_error(win_error(errno));
             c.ret(0);
             return;
         }
         if (c.arg(3) != 0) {
             mem::write<uint32_t>(c.arg(3), static_cast<uint32_t>(n));
         }
         if (overlapped != 0) {
             complete_overlapped(overlapped, static_cast<uint32_t>(n), n == 0 && c.arg(2) != 0 ? 0xc0000011u : 0);
             if (n == 0 && c.arg(2) != 0) {
                 set_last_error(kErrorHandleEof);
                 c.ret(0);
                 return;
             }
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "WriteFile", 0, 5, Conv::stdcall,
     [](ApiCall& c) {
         ssize_t n = 0;
         if (auto stream = handle_get<StdStream>(c.arg(0))) {
             n = static_cast<ssize_t>(std::fwrite(mem::ptr(c.arg(1)), 1, c.arg(2), stderr));
         } else if (auto file = handle_get<FileObject>(c.arg(0))) {
             uint32_t overlapped = c.arg(4);
             if (overlapped != 0) {
                 off_t offset = static_cast<off_t>(mem::read<uint32_t>(overlapped + 8)) |
                                (static_cast<off_t>(mem::read<uint32_t>(overlapped + 12)) << 32);
                 n = ::pwrite(file->fd, mem::ptr(c.arg(1)), c.arg(2), offset);
             } else {
                 n = ::write(file->fd, mem::ptr(c.arg(1)), c.arg(2));
             }
             if (n >= 0 && overlapped != 0) {
                 complete_overlapped(overlapped, static_cast<uint32_t>(n), 0);
             }
         } else {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         if (n < 0) {
             set_last_error(win_error(errno));
             c.ret(0);
             return;
         }
         if (c.arg(3) != 0) {
             mem::write<uint32_t>(c.arg(3), static_cast<uint32_t>(n));
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "GetFileType", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         if (handle_get<StdStream>(c.arg(0))) {
             c.ret(2);
         } else if (handle_get<FileObject>(c.arg(0))) {
             c.ret(1);
         } else {
             c.ret(0);
         }
     }},
    {"KERNEL32.dll", "GetOverlappedResult", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         uint32_t overlapped = c.arg(1);
         if (c.arg(2) != 0) {
             mem::write<uint32_t>(c.arg(2), mem::read<uint32_t>(overlapped + 4));
         }
         uint32_t status = mem::read<uint32_t>(overlapped);
         if (status != 0) {
             set_last_error(status == 0xc0000011u ? kErrorHandleEof : 31);
             c.ret(0);
             return;
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "SetFilePointer", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         auto file = handle_get<FileObject>(c.arg(0));
         if (!file) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0xffffffffu);
             return;
         }
         int64_t distance = static_cast<int32_t>(c.arg(1));
         if (c.arg(2) != 0) {
             distance = static_cast<int64_t>((static_cast<uint64_t>(mem::read<uint32_t>(c.arg(2))) << 32) | c.arg(1));
         }
         static constexpr int kWhence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
         off_t r = ::lseek(file->fd, distance, kWhence[c.arg(3) % 3]);
         if (r < 0) {
             set_last_error(131);
             c.ret(0xffffffffu);
             return;
         }
         if (c.arg(2) != 0) {
             mem::write<uint32_t>(c.arg(2), static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32));
         }
         set_last_error(0);
         c.ret(static_cast<uint32_t>(r));
     }},
    {"KERNEL32.dll", "GetFileSize", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto file = handle_get<FileObject>(c.arg(0));
         struct stat st {};
         if (!file || ::fstat(file->fd, &st) != 0) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0xffffffffu);
             return;
         }
         if (c.arg(1) != 0) {
             mem::write<uint32_t>(c.arg(1), static_cast<uint32_t>(static_cast<uint64_t>(st.st_size) >> 32));
         }
         c.ret(static_cast<uint32_t>(st.st_size));
     }},
    {"KERNEL32.dll", "SetEndOfFile", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto file = handle_get<FileObject>(c.arg(0));
         if (!file) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         off_t position = ::lseek(file->fd, 0, SEEK_CUR);
         c.ret(::ftruncate(file->fd, position) == 0 ? 1 : 0);
     }},
    {"KERNEL32.dll", "DeleteFileA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto host = host_path_for(guest_string(c.arg(0)));
         if (!host || ::unlink(host->c_str()) != 0) {
             set_last_error(host ? win_error(errno) : kErrorPathNotFound);
             c.ret(0);
             return;
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "CreateDirectoryA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto host = host_path_for(guest_string(c.arg(0)));
         if (!host) {
             set_last_error(kErrorPathNotFound);
             c.ret(0);
             return;
         }
         if (::mkdir(host->c_str(), 0755) != 0) {
             set_last_error(errno == EEXIST ? kErrorAlreadyExists : win_error(errno));
             c.ret(0);
             return;
         }
         c.ret(1);
     }},
    {"KERNEL32.dll", "GetFileAttributesA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         auto host = host_path_for(guest_string(c.arg(0)));
         struct stat st {};
         if (!host || ::stat(host->c_str(), &st) != 0) {
             set_last_error(kErrorFileNotFound);
             c.ret(kInvalidAttributes);
             return;
         }
         c.ret(attributes_of(st));
     }},
    {"KERNEL32.dll", "GetFullPathNameA", 0, 4, Conv::stdcall,
     [](ApiCall& c) {
         std::string full = normalize_guest_path(guest_string(c.arg(0)), current_directory());
         uint32_t need = static_cast<uint32_t>(full.size() + 1);
         if (c.arg(1) < need) {
             c.ret(need);
             return;
         }
         mem::write_cstr(c.arg(2), full);
         if (c.arg(3) != 0) {
             size_t slash = full.find_last_of('\\');
             mem::write<uint32_t>(c.arg(3), c.arg(2) + static_cast<uint32_t>(slash + 1));
         }
         c.ret(need - 1);
     }},
    {"KERNEL32.dll", "FindFirstFileA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::string pattern = normalize_guest_path(guest_string(c.arg(0)), current_directory());
         size_t slash = pattern.find_last_of('\\');
         std::string directory = pattern.substr(0, slash);
         std::string mask = pattern.substr(slash + 1);
         auto host = host_path_for(directory);
         auto find = std::make_shared<FindObject>();
         std::error_code ec;
         if (host) {
             for (std::filesystem::directory_iterator it(*host, ec), end; !ec && it != end; it.increment(ec)) {
                 if (wildcard_match(mask, it->path().filename().string())) {
                     find->entries.push_back(*it);
                 }
             }
         }
         if (find->entries.empty()) {
             set_last_error(kErrorFileNotFound);
             c.ret(kInvalidHandle);
             return;
         }
         fill_find_data(c.arg(1), find->entries[find->next++]);
         c.ret(handle_insert(find));
     }},
    {"KERNEL32.dll", "FindNextFileA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         auto find = handle_get<FindObject>(c.arg(0));
         if (!find || find->next >= find->entries.size()) {
             set_last_error(find ? kErrorNoMoreFiles : kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         fill_find_data(c.arg(1), find->entries[find->next++]);
         c.ret(1);
     }},
    {"KERNEL32.dll", "FindClose", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(handle_close(c.arg(0)) ? 1 : 0); }},
    {"KERNEL32.dll", "GetCurrentDirectoryA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         std::string cwd = current_directory();
         uint32_t need = static_cast<uint32_t>(cwd.size() + 1);
         if (c.arg(0) < need) {
             c.ret(need);
             return;
         }
         mem::write_cstr(c.arg(1), cwd);
         c.ret(need - 1);
     }},
    {"KERNEL32.dll", "SetCurrentDirectoryA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::string target = normalize_guest_path(guest_string(c.arg(0)), current_directory());
         auto host = host_path_for(target);
         std::error_code ec;
         if (!host || !std::filesystem::is_directory(*host, ec)) {
             std::fprintf(stderr, "[FILE] SetCurrentDirectoryA(\"%s\") failed\n", target.c_str());
             set_last_error(kErrorPathNotFound);
             c.ret(0);
             return;
         }
         set_current_directory(target);
         c.ret(1);
     }},
    {"KERNEL32.dll", "GetDiskFreeSpaceA", 0, 5, Conv::stdcall,
     [](ApiCall& c) {
         struct statvfs vfs {};
         uint64_t free_bytes = 0x7fffffffull;
         uint64_t total_bytes = 0x7fffffffull;
         if (::statvfs(process().game_dir.c_str(), &vfs) == 0) {
             free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
             total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
         }
         constexpr uint32_t kSectorsPerCluster = 8;
         constexpr uint32_t kBytesPerSector = 512;
         constexpr uint64_t kCluster = kSectorsPerCluster * kBytesPerSector;
         auto clamp = [](uint64_t v) { return static_cast<uint32_t>(std::min<uint64_t>(v, 0x7fffffffull / kCluster)); };
         if (c.arg(1)) mem::write<uint32_t>(c.arg(1), kSectorsPerCluster);
         if (c.arg(2)) mem::write<uint32_t>(c.arg(2), kBytesPerSector);
         if (c.arg(3)) mem::write<uint32_t>(c.arg(3), clamp(free_bytes / kCluster));
         if (c.arg(4)) mem::write<uint32_t>(c.arg(4), clamp(total_bytes / kCluster));
         c.ret(1);
     }},
    {"KERNEL32.dll", "GetDriveTypeA", 0, 1, Conv::stdcall,
     [](ApiCall& c) {
         std::string root = guest_string(c.arg(0));
         bool is_c = root.empty() || root[0] == 'C' || root[0] == 'c';
         c.ret(is_c ? 3 : 1);
     }},
    {"KERNEL32.dll", "GetLogicalDriveStringsA", 0, 2, Conv::stdcall,
     [](ApiCall& c) {
         static constexpr char kDrives[] = "C:\\\0";
         if (c.arg(0) < sizeof(kDrives)) {
             c.ret(sizeof(kDrives));
             return;
         }
         std::memcpy(mem::ptr(c.arg(1)), kDrives, sizeof(kDrives));
         c.ret(sizeof(kDrives) - 1);
     }},
    {"KERNEL32.dll", "GetVolumeInformationA", 0, 8, Conv::stdcall,
     [](ApiCall& c) {
         if (c.arg(1) && c.arg(2)) mem::write_cstr(c.arg(1), "MACANDREAS");
         if (c.arg(3)) mem::write<uint32_t>(c.arg(3), 0x4d414e44);
         if (c.arg(4)) mem::write<uint32_t>(c.arg(4), 255);
         if (c.arg(5)) mem::write<uint32_t>(c.arg(5), 0x00000007);
         if (c.arg(6) && c.arg(7)) mem::write_cstr(c.arg(6), "NTFS");
         c.ret(1);
     }},
    {"KERNEL32.dll", "CreateFileMappingA", 0, 6, Conv::stdcall,
     [](ApiCall& c) {
         auto file = handle_get<FileObject>(c.arg(0));
         if (!file) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         struct stat st {};
         ::fstat(file->fd, &st);
         auto mapping = std::make_shared<MappingObject>();
         mapping->file = file;
         mapping->size = c.arg(4) ? c.arg(4) : static_cast<uint32_t>(st.st_size);
         c.ret(handle_insert(mapping));
     }},
    {"KERNEL32.dll", "MapViewOfFile", 0, 5, Conv::stdcall,
     [](ApiCall& c) {
         auto mapping = handle_get<MappingObject>(c.arg(0));
         if (!mapping) {
             set_last_error(kErrorInvalidHandle);
             c.ret(0);
             return;
         }
         uint32_t offset = c.arg(3);
         uint32_t size = c.arg(4) ? c.arg(4) : mapping->size - offset;
         uint32_t view = allocator().reserve(size);
         if (view == 0) {
             set_last_error(8);
             c.ret(0);
             return;
         }
         ssize_t n = ::pread(mapping->file->fd, mem::ptr(view), size, offset);
         if (n < 0) {
             set_last_error(win_error(errno));
             c.ret(0);
             return;
         }
         if (c.arg(1) & 0x2) {
             std::fprintf(stderr, "[FILE] MapViewOfFile(\"%s\") with write access: writes are not persisted\n",
                          mapping->file->guest.c_str());
         }
         c.ret(view);
     }},
    {"KERNEL32.dll", "UnmapViewOfFile", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(1); }},
    {"KERNEL32.dll", "FindResourceA", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(find_resource(c.arg(0), c.arg(1), c.arg(2), false)); }},
    {"KERNEL32.dll", "FindResourceW", 0, 3, Conv::stdcall,
     [](ApiCall& c) { c.ret(find_resource(c.arg(0), c.arg(1), c.arg(2), true)); }},
    {"KERNEL32.dll", "LoadResource", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(1) ? process().image_base + mem::read<uint32_t>(c.arg(1)) : 0); }},
    {"KERNEL32.dll", "LockResource", 0, 1, Conv::stdcall, [](ApiCall& c) { c.ret(c.arg(0)); }},
    {"KERNEL32.dll", "SizeofResource", 0, 2, Conv::stdcall,
     [](ApiCall& c) { c.ret(c.arg(1) ? mem::read<uint32_t>(c.arg(1) + 4) : 0); }},
    {"KERNEL32.dll", "GetDateFormatA", 0, 6, Conv::stdcall,
     [](ApiCall& c) {
         std::string picture = c.arg(3) ? mem::read_cstr(c.arg(3)) : (c.arg(1) & 2 ? "dddd, MMMM dd, yyyy" : "M/d/yyyy");
         c.ret(write_formatted(format_picture(picture, read_systemtime(c.arg(2))), c.arg(4), c.arg(5)));
     }},
    {"KERNEL32.dll", "GetTimeFormatA", 0, 6, Conv::stdcall,
     [](ApiCall& c) {
         std::string picture = c.arg(3) ? mem::read_cstr(c.arg(3)) : (c.arg(1) & 2 ? "h:mm tt" : "h:mm:ss tt");
         c.ret(write_formatted(format_picture(picture, read_systemtime(c.arg(2))), c.arg(4), c.arg(5)));
     }},
};

}

std::span<const ApiDef> file_apis() { return kFiles; }

}
