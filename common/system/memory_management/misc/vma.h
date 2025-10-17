#pragma once

#include <vector>
#include <tuple>
#include <iostream>
#include <fstream>
#include "fixed_types.h"


struct Range
{
    IntPtr vpn;
    IntPtr bounds;
    IntPtr offset;
};

class VMA
{
    private:
        IntPtr vbase;
        IntPtr vend;
        std::vector<Range> physical_ranges;
        bool allocated;

    public:
        VMA(IntPtr base, IntPtr end){
            vbase = base;
            vend = end;
            allocated = false;
        }
        ~VMA()
        {
            physical_ranges.clear();
        }
        
        void addPhysicalRange(Range range){
            physical_ranges.push_back(range);
        }
        void setAllocated(bool alloc){
            allocated = alloc;
        }
        bool isAllocated(){
            return allocated;
        }
        IntPtr getBase(){
            return vbase;
        }
        IntPtr getEnd(){
            return vend;
        }
        std::vector<Range> getPhysicalRanges(){
            return physical_ranges;
        }
        void printVMA(){
            std::cout << "VMA: " << vbase << " - " << vend << std::endl;
            for (auto range : physical_ranges){
                std::cout << "Physical Range: " << range.vpn << " - " << range.bounds << " - " << range.offset << std::endl;
            }
        }

};

