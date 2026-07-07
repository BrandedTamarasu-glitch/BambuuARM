#include "bambu_networking.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

using create_agent_fn = void *(*)(std::string);
using destroy_agent_fn = int (*)(void *);
using set_config_dir_fn = int (*)(void *, std::string);
using start_send_fn = int (*)(void *, BBL::PrintParams, BBL::OnUpdateStatusFn, BBL::WasCancelledFn, BBL::OnWaitFn);

template <class T>
T sym(void *lib, const char *name)
{
    dlerror();
    auto *ptr = dlsym(lib, name);
    const char *err = dlerror();
    if (err || !ptr) {
        std::cerr << "missing symbol " << name << ": " << (err ? err : "null") << "\n";
        std::exit(2);
    }
    return reinterpret_cast<T>(ptr);
}

int main(int argc, char **argv)
{
    if (argc < 5 || argc > 6) {
        std::cerr << "usage: smoke-upload <local-file> <dev-ip> <dev-id> <access-code> [plugin-so]\n";
        return 2;
    }

    const std::string local_file = argv[1];
    const std::string dev_ip = argv[2];
    const std::string dev_id = argv[3];
    const std::string access_code = argv[4];
    const std::string plugin = argc > 5 ? argv[5] : "./build/libbambu_networking.so";

    void *lib = dlopen(plugin.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 2;
    }

    auto create_agent = sym<create_agent_fn>(lib, "bambu_network_create_agent");
    auto destroy_agent = sym<destroy_agent_fn>(lib, "bambu_network_destroy_agent");
    auto set_config_dir = sym<set_config_dir_fn>(lib, "bambu_network_set_config_dir");
    auto start_send = sym<start_send_fn>(lib, "bambu_network_start_send_gcode_to_sdcard");

    void *agent = create_agent("/tmp");
    set_config_dir(agent, std::string(std::getenv("HOME")) + "/.var/app/com.bambulab.BambuStudio/config/BambuStudio");

    BBL::PrintParams params{};
    params.dev_id = dev_id;
    params.dev_ip = dev_ip;
    params.username = "bblp";
    params.password = access_code;
    params.filename = local_file;
    params.ftp_folder = "cache/";
    params.use_ssl_for_ftp = true;
    params.use_ssl_for_mqtt = true;
    params.project_name = "smoke_upload";
    params.plate_index = 0;

    int rc = start_send(agent, params, {}, {}, {});
    destroy_agent(agent);
    dlclose(lib);

    std::cout << "start_send_gcode_to_sdcard rc=" << rc << "\n";
    return rc == 0 ? 0 : 1;
}
