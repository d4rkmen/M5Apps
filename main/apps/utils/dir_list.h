/**
 * @file dir_list.h
 * @brief Compact sorted directory listing with O(1) item access.
 *        Stores names in a flat buffer + small metadata array.
 *        Per-item cost: ~16 bytes + name length (vs ~150+ bytes for std::string-based structs).
 */
#pragma once
#include <cstdint>
#include <ctime>

namespace UTILS
{

class DirList
{
public:
    struct Item
    {
        const char* name;
        uint32_t size;
        time_t mtime;
        bool is_dir;
    };

    ~DirList() { clear(); }

    /// Scan directory, optionally filtering files by extension (e.g. ".bin"). Dirs always included.
    /// Sorted: directories first, then files, alphabetically within each group.
    void scan(const char* path, const char* ext_filter = nullptr);

    void clear();
    int count() const { return _count; }
    Item at(int index) const;

private:
    struct Slot
    {
        uint32_t name_off;
        uint32_t size;
        time_t mtime;
        bool is_dir;
    };

    char* _names = nullptr;
    Slot* _slots = nullptr;
    int _count = 0;
};

} // namespace UTILS
