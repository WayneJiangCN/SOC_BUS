#include "tm_ring_demo_test.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments
{
    std::string scenario_config =
        tm_ring_demo::kDefaultRingScenarioConfig;
    std::string case_name = "multi_core";
    std::string ddr_config;
    std::string output = "tm_ring_multi_core_result.txt";
    std::vector<std::string> overrides;
};

void print_help(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --config FILE       Ring scenario TOML\n"
        << "  --case NAME         multi_core or multi_core_backpressure\n"
        << "  --ddr-config FILE   Override DDR/PEM runtime TOML\n"
        << "  --pem-config FILE   Compatibility alias of --ddr-config\n"
        << "  --output FILE       Result text file\n"
        << "  --set KEY=VALUE     Override one scenario value (repeatable)\n";
}

bool take_value(int argc, char** argv, int* index, std::string* value,
                std::string* error)
{
    if (*index + 1 >= argc) {
        *error = std::string("missing value for ") + argv[*index];
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool parse_arguments(int argc, char** argv, Arguments* args,
                     std::string* error)
{
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--help" || option == "-h") {
            print_help(argv[0]);
            return false;
        }
        if (option == "--config") {
            if (!take_value(argc, argv, &i, &args->scenario_config, error))
                return false;
        } else if (option == "--case") {
            if (!take_value(argc, argv, &i, &args->case_name, error))
                return false;
        } else if (option == "--ddr-config" || option == "--pem-config") {
            if (!take_value(argc, argv, &i, &args->ddr_config, error))
                return false;
        } else if (option == "--output") {
            if (!take_value(argc, argv, &i, &args->output, error))
                return false;
        } else if (option == "--set") {
            std::string value;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            args->overrides.push_back(value);
        } else {
            *error = "unknown option: " + option;
            return false;
        }
    }
    return true;
}

bool apply_overrides(const std::vector<std::string>& overrides,
                     tm_ring_demo::RingDemoConfig* config,
                     std::string* error)
{
    for (const auto& item : overrides) {
        const size_t equal = item.find('=');
        if (equal == std::string::npos) {
            *error = "--set expects KEY=VALUE, got: " + item;
            return false;
        }
        if (!tm_ring_demo::apply_demo_value(
                item.substr(0, equal), item.substr(equal + 1), config, error)) {
            return false;
        }
    }
    return tm_ring_demo::validate_config(*config, error);
}

}  // namespace

int main(int argc, char** argv)
{
    Arguments args;
    std::string error;
    if (!parse_arguments(argc, argv, &args, &error)) {
        if (!error.empty()) {
            std::cerr << "argument error: " << error << std::endl;
            return 2;
        }
        return 0;
    }

    tm_ring_demo::RingDemoConfig config;
    if (!tm_ring_demo::load_demo_config(
            args.scenario_config, args.case_name, &config, &error) ||
        !apply_overrides(args.overrides, &config, &error)) {
        std::cerr << "configuration error: " << error << std::endl;
        return 2;
    }
    if (!args.ddr_config.empty()) {
        config.ddr_config_file = args.ddr_config;
    }

    try {
        return tm_ring_demo::run_demo_to_file<PemTrDemo>(
            config.ddr_config_file, config, args.output);
    } catch (const std::exception& ex) {
        std::cerr << "ring ESL run failed: " << ex.what() << std::endl;
        return 1;
    }
}
