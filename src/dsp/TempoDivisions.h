#pragma once

#include <algorithm>
#include <array>

// The divisions of the tempo anything can be locked to, in ONE table: its names and lengths are
// here and nowhere else. A list that offers divisions is a VIEW of this table (tempo::View): the
// entries it offers, by name, in the order it stores them.
//   - an effect's time or rate (FxSync, EffectBase.h): the whole table in this order, shortest first;
//   - an LFO's cycle (dsp/LFO.h): the whole table, its own seventeen first (longest first, as they
//     were stored), the five it gained after them.
// Both are stored by index in projects and presets, so neither order moves: a new division is
// appended here and at the end of every view that offers it (a view of the whole table says
// so, `whole()`, and its tests then demand the new entry).
// A bar is four beats; lengths are in beats (quarter notes).
namespace tempo
{
static constexpr int COUNT = 22;
static constexpr int QUARTER = 12;

inline constexpr const char* NAMES[COUNT] = {
    "1/128", "1/64", "1/32T", "1/32", "1/16T", "1/32D", "1/16", "1/8T", "1/16D", "1/8", "1/4T",
    "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/2D", "1BAR", "2BAR", "4BAR", "8BAR", "16BAR" };

inline constexpr double BEATS[COUNT] = {
    1.0 / 32, 1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 3.0 / 16, 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3,
    3.0 / 4, 1.0, 4.0 / 3, 1.5, 2.0, 3.0, 4.0, 8.0, 16.0, 32.0, 64.0 };

inline const char* const* labels() { return NAMES; }
inline double beats(int index) { return BEATS[std::clamp(index, 0, COUNT - 1)]; }

constexpr bool sameText(const char* a, const char* b)
{
    for (; *a != '\0' && *a == *b; ++a, ++b) {}
    return *a == *b;
}

// The entry with this label, or -1.
constexpr int indexOf(const char* label)
{
    if (label == nullptr) return -1;
    for (int i = 0; i < COUNT; ++i)
        if (sameText(NAMES[i], label)) return i;
    return -1;
}
inline int fromLabel(const char* label) { return indexOf(label); }

// Some entries of the table in an order of their own, named: what a cell offers and stores.
// Built at compile time (a view that names a division the table does not have, or one twice,
// fails `valid()`, which its owner static_asserts).
template <int N>
struct View
{
    std::array<int, N> entry {};            // view index -> table index
    std::array<const char*, N> name {};     // the table's own label strings (usable as a cell's choices)

    constexpr explicit View(const std::array<const char*, N>& labels)
    {
        for (int i = 0; i < N; ++i)
        {
            entry[(size_t) i] = indexOf(labels[(size_t) i]);
            name[(size_t) i] = entry[(size_t) i] >= 0 ? NAMES[entry[(size_t) i]] : nullptr;
        }
    }

    constexpr bool valid() const
    {
        for (int i = 0; i < N; ++i)
        {
            if (entry[(size_t) i] < 0) return false;
            for (int j = 0; j < i; ++j)
                if (entry[(size_t) j] == entry[(size_t) i]) return false;
        }
        return true;
    }
    // Every division of the table is offered.
    constexpr bool whole() const { return N == COUNT && valid(); }

    constexpr int size() const { return N; }
    constexpr int tableIndex(int i) const { return entry[(size_t) std::clamp(i, 0, N - 1)]; }
    constexpr double beatsAt(int i) const { return BEATS[tableIndex(i)]; }
    const char* const* labels() const { return name.data(); }
    // The view index of a table entry, or -1.
    constexpr int find(int tableEntry) const
    {
        for (int i = 0; i < N; ++i)
            if (entry[(size_t) i] == tableEntry) return i;
        return -1;
    }
};
}
