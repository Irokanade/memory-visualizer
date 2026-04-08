#include "event.h"
#include "renderer.h"

#include <cstdio>
#include <memory>

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("usage: %s <trace.bin>\n", argv[0]);
        return 1;
    }

    EventQueue *queue = read_trace(argv[1]);
    if (!queue) {
        printf("failed to read trace: %s\n", argv[1]);
        return 1;
    }
    printf("Loaded %llu events from %s\n", (unsigned long long)queue->count,
           argv[1]);

    auto r = std::make_unique<Renderer>();
    renderer_init(r.get(), queue, nullptr, 0);
    renderer_run(r.get());
    renderer_destroy(r.get());

    delete queue;
    return 0;
}
