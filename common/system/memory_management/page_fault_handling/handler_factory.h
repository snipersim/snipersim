// Create a factory class for page fault handlers
# pragma once
# include "page_fault_handler.h"
# include "physical_memory_allocator.h"
# include "page_fault_handler_base.h"
# include <string>


class HandlerFactory {

    public:
        // Create a page fault handler
        static PageFaultHandlerBase* createHandler(String handlerType, PhysicalMemoryAllocator *allocator, String name) {

            if (handlerType == "default") {
                return new PageFaultHandler(allocator, name);
            }
            else {
                return NULL;
            }
        }
};
