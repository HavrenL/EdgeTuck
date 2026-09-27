#pragma once
#include <vector>

namespace edge {
struct Slot {
    int id{};
    int start{};
    int span{3};
    int end() const { return start + span; }
    bool operator==(const Slot&) const = default;
};

// All coordinates are grid cells along one screen edge. Order is stable.
bool valid_layout(const std::vector<Slot>& slots, int capacity);
bool resize_end(std::vector<Slot>& slots, int id, int requested_end, int capacity, int minimum = 2);
bool resize_start(std::vector<Slot>& slots, int id, int requested_start, int capacity, int minimum = 2);
bool move_slot(std::vector<Slot>& slots, int id, int requested_start, int capacity);
int first_gap(const std::vector<Slot>& slots, int span, int capacity);
}
