#pragma once

#include "clipboard.h"

#include <algorithm>
#include <iterator>

namespace mcs {

inline constexpr int kPickerItemsPerKind = 5;

constexpr int HistoryLimit(ClipKind kind) {
    return kind == ClipKind::Text ? 10 : 5;
}

struct HistoryEntry {
    UINT id;
    std::shared_ptr<const ClipData> clip;
};

class History {
public:
    void Add(std::shared_ptr<const ClipData> clip) {
        const ClipKind kind = clip->kind;
        if (++nextId_ == 0) {
            ++nextId_;
        }
        entries_.insert(entries_.begin(), {nextId_, std::move(clip)});

        int count = 0;
        const int limit = HistoryLimit(kind);
        const auto oldest = std::find_if(entries_.begin(), entries_.end(), [kind, limit, &count](const HistoryEntry& entry) {
            return entry.clip->kind == kind && ++count > limit;
        });
        if (oldest != entries_.end()) {
            entries_.erase(oldest);
        }
    }

    bool Remove(UINT id) {
        return std::erase_if(entries_, [id](const HistoryEntry& entry) { return entry.id == id; }) > 0;
    }

    void Clear() { entries_.clear(); }
    const std::vector<HistoryEntry>& Entries() const { return entries_; }

private:
    UINT nextId_ = 0;
    std::vector<HistoryEntry> entries_;
};

}
