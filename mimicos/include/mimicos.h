
/* You need to make it simulator agnostic */
#pragma once
#include <string>
#include <iostream>
#include <fstream>
#include "INIReader.h"
#include "memory_management/physical_memory_allocators/physical_memory_allocator.h"
#include "globals.h"

// Forward declarations
class INIReader;
class MetricsRegistry;
class PhysicalMemoryAllocator;
struct mm_package;

struct Message{
    int argc;
    uint64_t *argv;
};

  
class MimicOS {
    private:    
    
        INIReader* reader;
        MetricsRegistry* m_stats;
        PhysicalMemoryAllocator* physical_memory_allocator;
        // Shared memory variables used to communicate with the simulator
        mm_package* mmpackage;
        
        //Declare Singleton
        static MimicOS* instance;
    public: 
        
        // Path to the output file to keep track of statistics
        std::string path_to_outputFile;
        // Path to the application file
        std::string path_to_app;
        // Path to the configuration file
        std::string path_to_configFile;

        MimicOS(std::string configurationFile, std::string outputFile, std::string appFile);
        void initHandlers();
        void boot();
        void setupSharedMemory();
        void start_application();
        void poll_for_signal();


        

        /* The application thread currently scheduled on this kernel, as an
           AppReaderPool id, or 0 when none is.  Used to resume the right one
           when its quantum expires. */
        uint64_t m_running_atid = 0;
        mm_package* get_mm_package(){ return mmpackage; }
        PhysicalMemoryAllocator* get_physical_memory_allocator(){ return physical_memory_allocator; }

        // Function to get the instance of the MimicOS class with the configuration file, output file, and application file
        static MimicOS* getMimicOS(std::string configurationFile, std::string outputFile, std::string appFile){
            if(instance == NULL){
                instance = new MimicOS(configurationFile, outputFile, appFile);
            }
            return instance;
        }

        // Overloaded getMimicOS function to get the instance of the MimicOS class without passing any arguments
        static MimicOS* getMimicOS(){
            return instance;
        }


        void handleDeallocate();
        void handlePrintAllocator();
        void handleInitRandom();
                
};