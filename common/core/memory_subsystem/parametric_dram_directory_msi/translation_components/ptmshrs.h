#pragma once
#include "subsecond_time.h"

struct MSHREntry {
    SubsecondTime request_time;
    SubsecondTime completion_time;
};

class MSHR {
public:
    MSHR(size_t max_entries) : max_entries(max_entries) {}

    bool canAllocate() const {
        return entries.size() < max_entries;
    }

    void allocate(MSHREntry& entry) {

        // Keep track of the request that completes first so that we can allocate the slot for the new request accordingly
        if (entry.completion_time < min_time) {
                min_time = entry.completion_time;
        }

        entries.push(entry);

        return;
    }

    SubsecondTime getSlotAllocationDelay(SubsecondTime request_incoming) {


        // |====================| -> min time
        // request_time -> |==================|	
        // 					 New Request
        SubsecondTime delay = SubsecondTime::Zero();


        if (entries.empty()) {
            return SubsecondTime::Zero();
        }

        if ( (entries.size()+1) > max_entries) {
    
                if (request_incoming < min_time) {
                    delay = min_time - request_incoming;
                }
        }

        return delay;
    }


    bool isEmpty() const {
        return entries.empty();
    }

    MSHREntry& front() {
        return entries.front();  
    }

    
    void removeCompletedEntries(SubsecondTime now) {
        while (!entries.empty() && entries.front().completion_time <= now) {
            entries.pop();
        }
    }

    size_t getOccupancy() const {
        return entries.size();
    }

private:
    size_t max_entries;
    std::queue<MSHREntry> entries;
    SubsecondTime max_time;
    SubsecondTime min_time;
};