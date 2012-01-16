
#include <iostream>
#include "config_file.hpp"

using namespace config;

int main(int argc, char **argv)
{
    ConfigFile cfg;
    cfg.load("test.cfg");

    cout << cfg.showFullTree() << endl;
}

