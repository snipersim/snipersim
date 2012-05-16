
#include <iostream>
#include "config_file.hpp"
#include "config.hpp"

using namespace config;

int main(int argc, char **argv)
{
    ConfigFile cfg;
    cfg.load("test.cfg");

    std::cerr << cfg.showFullTree() << std::endl;
    std::cerr << cfg.getFloatArray("section/key4", 0) << std::endl;;
    std::cerr << cfg.getFloatArray("section/key4", 1) << std::endl;;
    std::cerr << cfg.getFloatArray("section/key4", 2) << std::endl;;
    std::cerr << cfg.getFloatArray("section/key4", 3) << std::endl;;

    cfg.set("section/key5", 5.8);
    std::cerr << cfg.getFloatArray("section/key5", 6) << std::endl;;
    std::cerr << cfg.getFloatArray("section/key5", 6) << std::endl;;
}

