#include "layout.hpp"
#include <algorithm>

namespace edge {
bool valid_layout(const std::vector<Slot>& slots, int capacity) {
    int end = 0;
    for (const auto& s : slots) {
        if (s.start < end || s.span < 1 || s.end() > capacity) return false;
        end = s.end();
    }
    return capacity >= 0;
}
static int index_of(const std::vector<Slot>& slots, int id) {
    for (size_t i = 0; i < slots.size(); ++i) if (slots[i].id == id) return static_cast<int>(i);
    return -1;
}
bool resize_end(std::vector<Slot>& slots, int id, int requested_end, int capacity, int minimum) {
    const int index = index_of(slots, id);
    if (index < 0 || !valid_layout(slots, capacity) || minimum < 1) return false;
    int maximum = capacity;
    for (size_t i = index + 1; i < slots.size(); ++i) maximum -= slots[i].span;
    auto& item = slots[index];
    if (maximum < item.start + minimum) return false;
    item.span = std::clamp(requested_end, item.start + minimum, maximum) - item.start;
    for (size_t i = index + 1; i < slots.size(); ++i) slots[i].start = std::max(slots[i].start, slots[i - 1].end());
    return true;
}
bool resize_start(std::vector<Slot>& slots, int id, int requested_start, int capacity, int minimum) {
    const int index = index_of(slots, id);
    if (index < 0 || !valid_layout(slots, capacity) || minimum < 1) return false;
    int lowest = 0;
    for (int i = 0; i < index; ++i) lowest += slots[i].span;
    auto& item = slots[index];
    const int end = item.end();
    if (end - minimum < lowest) return false;
    item.start = std::clamp(requested_start, lowest, end - minimum);
    item.span = end - item.start;
    for (int i = index - 1; i >= 0; --i) slots[i].start = std::min(slots[i].start, slots[i + 1].start - slots[i].span);
    return true;
}
bool move_slot(std::vector<Slot>& slots, int id, int requested_start, int capacity) {
    const int index = index_of(slots, id);
    if (index < 0 || !valid_layout(slots, capacity)) return false;
    int lowest = 0, highest = capacity - slots[index].span;
    for (int i = 0; i < index; ++i) lowest += slots[i].span;
    for (size_t i = index + 1; i < slots.size(); ++i) highest -= slots[i].span;
    const int old_start = slots[index].start;
    slots[index].start = std::clamp(requested_start, lowest, highest);
    if (slots[index].start < old_start) {
        for (int i = index - 1; i >= 0; --i) slots[i].start = std::min(slots[i].start, slots[i + 1].start - slots[i].span);
    } else {
        for (size_t i = index + 1; i < slots.size(); ++i) slots[i].start = std::max(slots[i].start, slots[i - 1].end());
    }
    return true;
}
int first_gap(const std::vector<Slot>& slots, int span, int capacity) {
    if (span < 1 || !valid_layout(slots, capacity)) return -1;
    int start = 0;
    for (const auto& slot : slots) {
        if (slot.start - start >= span) return start;
        start = slot.end();
    }
    return start + span <= capacity ? start : -1;
}
}
