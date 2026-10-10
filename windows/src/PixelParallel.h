#pragma once
#include <atomic>
#include <cstddef>
#include <ppl.h>
template <class Body> void pixel_parallel(size_t count, Body body) {
    if (count <= 1) {
        if (count)
            body(size_t(0));
        return;
    }
    concurrency::parallel_for(size_t(0), count, body);
}
