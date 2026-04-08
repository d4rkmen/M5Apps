/**
 * @file dir_list.cpp
 * @brief Compact sorted directory listing implementation.
 */
#include "dir_list.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

namespace UTILS
{

void DirList::clear()
{
    free(_names);
    _names = nullptr;
    free(_slots);
    _slots = nullptr;
    _count = 0;
}

DirList::Item DirList::at(int index) const
{
    auto& s = _slots[index];
    return {_names + s.name_off, s.size, s.mtime, s.is_dir};
}

void DirList::scan(const char* path, const char* ext_filter)
{
    clear();

    DIR* dir = opendir(path);
    if (!dir)
        return;

    // Pass 1: count entries and total name bytes
    int dir_count = 0, file_count = 0;
    size_t name_bytes = 0;
    struct dirent* entry;

    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        std::string full_path = std::string(path) + "/" + entry->d_name;
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0)
            continue;

        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && ext_filter)
        {
            size_t nlen = strlen(entry->d_name);
            size_t elen = strlen(ext_filter);
            if (nlen < elen)
                continue;
            bool match = true;
            for (size_t i = 0; i < elen; i++)
            {
                char a = entry->d_name[nlen - elen + i];
                char b = ext_filter[i];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (b >= 'A' && b <= 'Z')
                    b += 32;
                if (a != b)
                {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;
        }

        if (is_dir)
            dir_count++;
        else
            file_count++;
        name_bytes += strlen(entry->d_name) + 1;
    }

    int total = dir_count + file_count;
    if (total == 0)
    {
        closedir(dir);
        return;
    }

    _slots = (Slot*)malloc(total * sizeof(Slot));
    _names = (char*)malloc(name_bytes);
    if (!_slots || !_names)
    {
        clear();
        closedir(dir);
        return;
    }

    // Pass 2: fill slots — dirs first, then files
    rewinddir(dir);
    int di = 0, fi = dir_count;
    size_t name_off = 0;

    // Temp parallel array for sorting: store name_off per slot so we can sort by name
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        std::string full_path = std::string(path) + "/" + entry->d_name;
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0)
            continue;

        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && ext_filter)
        {
            size_t nlen = strlen(entry->d_name);
            size_t elen = strlen(ext_filter);
            if (nlen < elen)
                continue;
            bool match = true;
            for (size_t i = 0; i < elen; i++)
            {
                char a = entry->d_name[nlen - elen + i];
                char b = ext_filter[i];
                if (a >= 'A' && a <= 'Z')
                    a += 32;
                if (b >= 'A' && b <= 'Z')
                    b += 32;
                if (a != b)
                {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;
        }

        size_t nlen = strlen(entry->d_name);
        memcpy(_names + name_off, entry->d_name, nlen + 1);

        int idx = is_dir ? di++ : fi++;
        _slots[idx] = {(uint32_t)name_off, (uint32_t)st.st_size, st.st_mtime, is_dir};
        name_off += nlen + 1;
    }
    closedir(dir);
    _count = total;

    // Sort each group alphabetically by name (case-insensitive)
    auto cmp = [this](const Slot& a, const Slot& b)
    { return strcasecmp(_names + a.name_off, _names + b.name_off) < 0; };

    if (dir_count > 1)
        std::sort(_slots, _slots + dir_count, cmp);
    if (file_count > 1)
        std::sort(_slots + dir_count, _slots + total, cmp);
}

} // namespace UTILS
