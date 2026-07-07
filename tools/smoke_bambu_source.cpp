#include <dlfcn.h>

#include <iostream>
#include <string>

using Bambu_Tunnel = void *;
using CreateFn = int (*)(Bambu_Tunnel *, const char *);
using OpenFn = int (*)(Bambu_Tunnel);
using LastErrorFn = const char *(*)();
using CloseFn = void (*)(Bambu_Tunnel);
using DestroyFn = void (*)(Bambu_Tunnel);

template <typename T>
T load_symbol(void *lib, const char *name)
{
    auto *symbol = dlsym(lib, name);
    if (!symbol)
        std::cerr << "missing symbol: " << name << "\n";
    return reinterpret_cast<T>(symbol);
}

int main(int argc, char **argv)
{
    const char *lib_path = argc > 1 ? argv[1] : "./build/libBambuSource.so";
    const char *host = argc > 2 ? argv[2] : "127.0.0.1";
    void *lib = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }

    auto create = load_symbol<CreateFn>(lib, "Bambu_Create");
    auto open = load_symbol<OpenFn>(lib, "Bambu_Open");
    auto last_error = load_symbol<LastErrorFn>(lib, "Bambu_GetLastErrorMsg");
    auto close = load_symbol<CloseFn>(lib, "Bambu_Close");
    auto destroy = load_symbol<DestroyFn>(lib, "Bambu_Destroy");
    if (!create || !open || !last_error || !close || !destroy)
        return 1;

    Bambu_Tunnel tunnel = nullptr;
    std::string url = std::string("bambu://") + host;
    int create_rc = create(&tunnel, url.c_str());
    int open_rc = tunnel ? open(tunnel) : -999;
    std::cout << "create_rc=" << create_rc << " open_rc=" << open_rc
              << " last_error=" << last_error() << "\n";

    if (tunnel) {
        close(tunnel);
        destroy(tunnel);
    }

    return create_rc == 0 && open_rc != -2 ? 0 : 1;
}
