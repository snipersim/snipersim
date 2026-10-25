
#pragma once
#include <string>
#include <iostream>

class ArgumentParser {
public:
    std::string configFile;
    std::string siftFile;
    std::string outputFile;

    ArgumentParser() {}

    bool parse(int argc, char* argv[]) {
        if (argc != 4) {
            std::cerr << "Usage: " << argv[0] << " <config_file> <sift_file> <output_file>" << std::endl;
            return false;
        }
        configFile = argv[1];
        siftFile = argv[2];
        outputFile = argv[3];

        // Further validation can be added here, such as checking file existence or path validity
        return true;
    }
};