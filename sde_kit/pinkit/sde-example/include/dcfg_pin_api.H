// 
// Copyright (C) 2015-2016 Intel Corporation.
// SPDX-License-Identifier: MIT
// 
#ifndef DCFG_PIN_API_H
#define DCFG_PIN_API_H

#include "pin.H"
#include "dcfg_api.H"

namespace dcfg_pin_api {

    /**
     * Connection between a Pin tool and a DCFG_DATA object.
     * This is an interface; use DCFG_PIN_MANAGER::new_manager()
     * to create an object that implements the interface.
     */
    class DCFG_PIN_MANAGER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_PIN_MANAGER() {}

        /**
         * Create a new DCFG_PIN_MANAGER. This is a factory method to create
         * a new object that implements the DCFG_PIN_MANAGER interface.
         * @return Pointer to new object. It can be freed with `delete`.
         */
        static DCFG_PIN_MANAGER* new_manager();

        /**
         * Whether the '-dcfg' knob was used on the command-line.
         * @return `true` if '-dcfg' knob was used, `false` otherwise.
         */
        virtual bool dcfg_enable_knob() const;

        /**
         * Initialize and add Pin instrumentation.  Default behavior depends
         * on settings of dcfg knobs.
         */
        virtual void activate() =0;

        /**
         * Initialize and add PinPlay instrumentation.  Default behavior
         * depends on settings of dcfg knobs and whether logger, replayer or
         * both are activated.
         */
        virtual void activate(void * pinplay_engine
                              /**< [in] pointer to existing pinplay engine
                                 or `NULL` if none. */
                              ) =0;

        /**
         * Get access to DCFG data constructed
         * by the Pin tool.
         * The returned DCFG will only be valid
         * at the end of a region or program.
         * There will not be a DCFG if the activate() method
         * has not been called.
         * @return Pointer to associated DCFG data or `NULL` if none.
         */
        virtual dcfg_api::DCFG_DATA_CPTR get_dcfg_data() const =0;
        
        /**
         * Explicitly set CFG-data collection.  This controls whether
         * control-flow instructions are instrumented to build a CFG.  This
         * is independent of whether a DCFG file is written.  If a DCFG file
         * is written with CFG collection disabled, it will have no CFG data
         * in it.
         */
        virtual void
        set_cfg_collection(bool enable
                           /**< [in] turn CFG collection on or off. */
                           ) =0;
    };

};                           // namespace
#endif
