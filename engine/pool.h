// Numbered handles for everything a game holds: textures, meshes, models, sounds, bodies.
// A handle is checked every time it is used, so an old or made-up one is refused instead of crashing.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

template <class T>
struct Pool {
    struct Slot {
        T item;
        uint16_t gen = 1;
        bool used = false;
    };
    std::vector<Slot> slots;

    uint32_t add(T item) {
        size_t i = 0;
        while (i < slots.size() && slots[i].used) i++;
        if (i == slots.size()) slots.emplace_back();
        slots[i].item = std::move(item);
        slots[i].used = true;
        return ((uint32_t)slots[i].gen << 16) | (uint32_t)(i + 1);
    }
    // Null for a handle that was never given out or has been freed. Adding an item can move the others.
    T* get(uint32_t handle) {
        uint32_t i = handle & 0xFFFF;
        if (i == 0 || i > slots.size()) return nullptr;
        Slot& s = slots[i - 1];
        return (s.used && s.gen == (handle >> 16)) ? &s.item : nullptr;
    }
    void remove(uint32_t handle) {
        if (!get(handle)) return;
        Slot& s = slots[(handle & 0xFFFF) - 1];
        s.used = false;
        s.item = T();
        if (++s.gen == 0) s.gen = 1;
    }
    template <class F>
    void each(F f) {
        for (size_t i = 0; i < slots.size(); i++)
            if (slots[i].used) f(((uint32_t)slots[i].gen << 16) | (uint32_t)(i + 1), slots[i].item);
    }
};
