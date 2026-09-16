#include <iostream>
#include <vector>
#include <memory>
#include <cstdint>

class RadixPageTableFrame {
public:
    static const int ENTRIES = 512; // Each table has 512 entries
    std::unique_ptr<uint64_t[]> entries;

    RadixPageTableFrame() : entries(new uint64_t[ENTRIES]) {
        std::fill_n(entries.get(), ENTRIES, 0);
    }
};

class RadixPageTable{

    private:
        std::vector<std::unique_ptr<RadixPageTableFrame>> pageTables;

        int getIndex(uint64_t address, int level) {
            return (address >> (12 + 9 * level)) & 0x1FF;
        }

    public:
        RadixPageTable(); 
        uint64_t* lookup(uint64_t virtualAddress);
        void insert(uint64_t virtualAddress, uint64_t physicalAddress);

};