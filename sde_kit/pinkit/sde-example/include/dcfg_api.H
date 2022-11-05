// 
// Copyright (C) 2015-2016 Intel Corporation.
// SPDX-License-Identifier: MIT
// 

#ifndef DCFG_API_H
#define DCFG_API_H

#include <vector>
#include <set>
#include <iostream>

// Fundamental types.
#if !defined(TYPES_FOUNDATION_H)

#if defined(_MSC_VER)
    typedef unsigned char    UINT8 ;
    typedef unsigned short   UINT16;
    typedef unsigned int     UINT32;
    typedef unsigned __int64 UINT64;
    typedef signed char      INT8;
    typedef signed short     INT16;
    typedef signed int       INT32;
    typedef signed __int64   INT64;
#else
#include <stdint.h>
    typedef uint8_t  UINT8;
    typedef uint16_t UINT16;
    typedef uint32_t UINT32;
    typedef uint64_t UINT64;
    typedef int8_t  INT8;
    typedef int16_t INT16;
    typedef int32_t INT32;
    typedef int64_t INT64;
#endif
#endif

namespace dcfg_api {

    /** \mainpage 
     * \section intro_sec Introduction
     *
     * A control-flow graph (CFG) is a fundamental structure used in
     * computer science and engineering for describing and analyzing the
     * structure of an algorithm or program. A dynamic control-flow graph
     * (DCFG) is a specialized CFG that adds data from a specific execution
     * of a program. This application-programmer interface (API) provides
     * access to the DCFG data from within a Pin tool or a standalone
     * program.
     */
 
    /**
     * Type for a generic ID number.
     * Used for basic blocks, edges, etc.
     */
    typedef UINT32 DCFG_ID;

    // Interface declarations.
    class DCFG_DATA;
    class DCFG_PROCESS;
    class DCFG_IMAGE;
    class DCFG_ROUTINE;
    class DCFG_LOOP;
    class DCFG_BASIC_BLOCK;
    class DCFG_EDGE;
    class DCFG_ID_CONTAINER;

    /** Pointer to a const DCFG_DATA interface */
    typedef const DCFG_DATA* DCFG_DATA_CPTR;

    /** Pointer to a const DCFG_PROCESS interface */
    typedef const DCFG_PROCESS* DCFG_PROCESS_CPTR;

    /** Pointer to a const DCFG_IMAGE interface */
    typedef const DCFG_IMAGE* DCFG_IMAGE_CPTR;

    /** Pointer to a const DCFG_ROUTINE interface */
    typedef const DCFG_ROUTINE* DCFG_ROUTINE_CPTR;

    /** Pointer to a const DCFG_LOOP interface */
    typedef const DCFG_LOOP* DCFG_LOOP_CPTR;

    /** Pointer to a const DCFG_BASIC_BLOCK interface */
    typedef const DCFG_BASIC_BLOCK* DCFG_BASIC_BLOCK_CPTR;

    /** Pointer to a const DCFG_EDGE interface */
    typedef const DCFG_EDGE* DCFG_EDGE_CPTR;

    /**
     * Interface to all data in a DCFG.
     * This is an interface; use DCFG_DATA::new_dcfg()
     * to create an object that implements the interface.
     */
    class DCFG_DATA {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_DATA() {}

        /**
         * Create a new DCFG.  This is a factory method to create a new
         * object that implements the DCFG_DATA interface.
         * @return Pointer to new object. It can be freed
         * with `delete`.
         */
        static DCFG_DATA* new_dcfg();

        /**
         * Set internal DCFG data from a C++ `istream`.
         * @return `true` on success, `false` otherwise (and sets `errMsg`).
         */
        virtual bool
        read(std::istream& strm,
             /**< [in] Stream to read from. Reads one JSON value, which must
                follow the DCFG JSON format. */
             std::string& errMsg,
             /**< [out] Contains error message upon failure. */
             bool readToEof = true
             /**< [in] Defines what to do after the JSON value is read.  If
                `true`, continue reading to end of input stream and fail if
                any non-whitespace characters are found.  If `false`, stop
                reading after the the JSON value.*/
             ) =0;

        /**
         * Open a file for reading and
         * set internal DCFG data from its contents.
         * @return `true` on success, `false` otherwise (and sets `errMsg`).
         */
        virtual bool
        read(const std::string filename,
             /**< [in] Name of file to open. Reads one JSON value, which
                must follow the DCFG JSON format. */
             std::string& errMsg
             /**< [out] Contains error message upon failure. */
             ) =0;

        /**
         * Write internal DCFG data to a C++ `ostream`.
         */
        virtual void
        write(std::ostream& strm
              /**< [out] Stream to write to.  Output will conform to the
                 DCFG JSON format. */
              ) const =0;

        /**
         * Write internal DCFG data to a file.
         * @return `true` on success, `false` otherwise (and sets `errMsg`).
         */
        virtual bool
        write(const std::string& filename,
              /**< [in] Name of file to open.  Output will conform to the
                 DCFG JSON format. */
              std::string& errMsg
              /**< [out] Contains error message upon failure. */
              ) const =0;

        /**
         * Set all dynamic counts to zero.
         */
        virtual void clearCounts() =0;

        /**
         * Get list of process IDs.
         * @return Number of IDs that were added to `process_ids`.
         */
        virtual UINT32
        get_process_ids(DCFG_ID_CONTAINER &process_ids
                        /**< [out] Container to which process IDs are added.
                           Previous contents of the container are *not*
                           emptied by this call, so it should be emptied by
                           the programmer before the call if desired.  The
                           programmer can use any implementation of
                           DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                           etc. */
                        ) const =0;

        /**
         * Get access to data for a process.
         * @return Pointer to interface object for specified process or `NULL`
         * if `process_id` is invalid.
         */
        virtual DCFG_PROCESS_CPTR
        get_process_info(DCFG_ID process_id
                         /**< [in] ID of desired process. */
                         ) const =0;
    };

    /**
     * Common interface to any structure containing nodes and edges between them,
     * i.e., processes, images, routines, loops and basic blocks.  A single
     * basic block is a special case consisting of one block and no internal
     * edges.  Most nodes correspond to basic blocks of the executed binary,
     * but some nodes are "special".  See the DCFG documentation for more
     * information.
     */
    class DCFG_GRAPH_BASE {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_GRAPH_BASE() {}

        /**
         * Get IDs of all basic blocks in the structure.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_basic_block_ids(DCFG_ID_CONTAINER &node_ids
                            /**< [out] Container to which IDs
                               are added.  Previous contents of the
                               container are *not* emptied by this call, so
                               it should be emptied by the programmer before
                               the call if desired.  The programmer can use
                               any implementation of DCFG_ID_CONTAINER:
                               DCFG_ID_VECTOR, DCFG_ID_SET, etc. */
                            ) const =0;

        /**
         * Get list of internal edge IDs.
         * These are all edges such that both the source and target
         * nodes are within the structure.
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_internal_edge_ids(DCFG_ID_CONTAINER &edge_ids
                              /**< [out] Container to which IDs are added.
                                 Previous contents of the container are *not* emptied
                                 by this call, so it should be emptied by the
                                 programmer before the call if desired.  The
                                 programmer can use any implementation of
                                 DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                 etc. */
                              ) const =0;

        /**
         * Get list of in-bound edge IDs.
         * These are all edges such that the source node *is not* within the structure
         * and the target node *is* within the structure.
         * Note that this set contains *all* the edges that terminate within
         * the structure, including returns from calls, interrupts, etc., not only those
         * that are considered to "enter" the structure.
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_inbound_edge_ids(DCFG_ID_CONTAINER &edge_ids
                             /**< [out] Container to which IDs are added.
                                Previous contents of the container are *not* emptied
                                by this call, so it should be emptied by the
                                programmer before the call if desired.  The
                                programmer can use any implementation of
                                DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                etc. */
                             ) const =0;


        /**
         * Get list of out-bound edge IDs.
         * These are all edges such that the source node *is* within the structure
         * and the target node *is not* within the structure.
         * Note that this set contains *all* the edges that originate within
         * the structure, including calls, interrupts, etc., not only those
         * that are considered to "exit" the structure.
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_outbound_edge_ids(DCFG_ID_CONTAINER &edge_ids
                              /**< [out] Container to which edge IDs are added.
                                 Previous contents of the container are *not* emptied
                                 by this call, so it should be emptied by the
                                 programmer before the call if desired.  The
                                 programmer can use any implementation of
                                 DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                 etc. */
                              ) const =0;

        /**
         * Get the total dynamic instruction count.
         * @return Count of instructions executed in this structure across
         * all threads.
         */
        virtual UINT64 get_instr_count() const =0;

        /**
         * Get per-thread dynamic instruction count.
         * @return Count of instructions executed in this structure on
         * specified thread or zero (0) if thread is invalid.
         */
        virtual UINT64
        get_instr_count_for_thread(UINT32 thread_id
                                   /**< [in] Thread number.  Typically, threads
                                    * are consecutively numbered from zero to
                                    * DCFG_PROCESS::get_highest_thread_id(). */
                                   ) const =0;
    };

    /**
     * Common interface to any structure containing loops, i.e.,
     * routines, images, and processes.
     * Note: even though loops can be nested, a loop is 
     * not considered a DCFG_LOOP_CONTAINER.
     * Loop nesting structure can be determined by
     * querying the parent loop id from a DCFG_LOOP object.
     */
    class DCFG_LOOP_CONTAINER :
        public virtual DCFG_GRAPH_BASE {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_LOOP_CONTAINER() {}

        /**
         * Get the set of loop IDs.
         * Get IDs of all loops in the structure.
         * A loop ID is the same as the ID of the basic block
         * at its head node.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_loop_ids(DCFG_ID_CONTAINER &node_ids
                     /**< [out] Container to which IDs are added.
                        Previous contents of the container are *not* emptied
                        by this call, so it should be emptied by the
                        programmer before the call if desired.  The
                        programmer can use any implementation of
                        DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                        etc. */
                     ) const =0;

        /**
         * Get access to data for a loop.
         * @return Pointer to interface object for specified loop or `NULL`
         * if `loop_id` is invalid.
         */
        virtual DCFG_LOOP_CPTR
        get_loop_info(DCFG_ID loop_id
                         /**< [in] ID of desired loop. */
                         ) const =0;
    };

    /**
     * Common interface to any structure containing routines, i.e.,
     * images and processes.
     */
    class DCFG_ROUTINE_CONTAINER :
        public virtual DCFG_LOOP_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_ROUTINE_CONTAINER() {}

        /**
         * Get the set of routine IDs.
         * Get IDs of all routines in the structure.
         * A routine ID is the same as the ID of the basic block
         * at its entry node.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_routine_ids(DCFG_ID_CONTAINER &node_ids
                     /**< [out] Container to which IDs are added.
                        Previous contents of the container are *not* emptied
                        by this call, so it should be emptied by the
                        programmer before the call if desired.  The
                        programmer can use any implementation of
                        DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                        etc. */
                     ) const =0;

        /**
         * Get access to data for a routine.
         * @return Pointer to interface object for specified routine or `NULL`
         * if `routine_id` is invalid.
         */
        virtual DCFG_ROUTINE_CPTR
        get_routine_info(DCFG_ID routine_id
                         /**< [in] ID of desired routine. */
                         ) const =0;
    };

    /**
     * Common interface to any structure containing images, i.e., 
     * processes.
     */
    class DCFG_IMAGE_CONTAINER :
        public virtual DCFG_ROUTINE_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_IMAGE_CONTAINER() {}

        /**
         * Get the set of image IDs.
         * Get IDs of all images seen, not just the ones that are active
         * at any given time. The address ranges of two or more of the
         * the images may overlap if an image was loaded after another
         * was unloaded.
         * @return Number of IDs that were added to `image_ids`.
         */
        virtual UINT32
        get_image_ids(DCFG_ID_CONTAINER &image_ids
                        /**< [out] Container to which image IDs are added.
                           Previous contents of the container are *not*
                           emptied by this call, so it should be emptied by
                           the programmer before the call if desired.  The
                           programmer can use any implementation of
                           DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                           etc. */
                        ) const =0;

        /**
         * Get access to data for an image.
         * @return Pointer to interface object for specified image or `NULL`
         * if `image_id` is invalid.
         */
        virtual DCFG_IMAGE_CPTR
        get_image_info(DCFG_ID image_id
                         /**< [in] ID of desired image. */
                         ) const =0;
    };

    /**
     * Interface to information about an O/S process.
     */
    class DCFG_PROCESS :
        public virtual DCFG_IMAGE_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_PROCESS() {}

        /**
         * Get the process ID.
         * @return Process ID captured when the DCFG was created.
         */
        virtual DCFG_ID get_process_id() const =0;

        /**
         * Get the highest thread ID.  The lowest thread ID is zero (0).
         * Typically, threads are consecutively numbered from zero to
         * DCFG_PROCESS::get_highest_thread_id().
         * @return Highest thread ID
         * recorded when the DCFG was created.
         */
        virtual UINT32 get_highest_thread_id() const =0;

        /**
         * Get basic block ID(s) containing given address
         * in this process.
         * It is possible to get zero or more IDs returned:
         * zero if the address appears in no basic blocks,
         * one if it appears in exactly one block in one image,
         * and more than one if it is not unique.
         * Basic blocks may not be unique if a
         * dynamically-linked process unloads one image and
         * loads another image in an overlapping address region.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_basic_block_ids_by_addr(UINT64 addr,
                                   /**< [in] Virtual address that can appear
                                      anywhere within a basic block. */
                                   DCFG_ID_CONTAINER &node_ids
                                   /**< [out] Container to which basic-block IDs are added.
                                      Previous contents of the container are *not*
                                      emptied by this call, so it should be emptied by
                                      the programmer before the call if desired.  The
                                      programmer can use any implementation of
                                      DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                      etc. */
                                    ) const =0;

        /**
         * Get ID of start node.
         * This is a "special" node that is not a basic block.
         * It is the source node of the edge to the first basic
         * block executed in each thread.
         * @return ID number of start node.
         */
        virtual UINT32 get_start_node_id() const =0;

        /**
         * Get ID of end node.
         * This is a "special" node that is not a basic block.
         * It is the target node of the edge from the last basic
         * block executed in each thread.
         * @return ID number of end node.
         */
        virtual UINT32 get_end_node_id() const =0;

        /**
         * Get ID of unknown node.
         * This is a "special" node that is not a basic block.
         * It is a placeholder for any section of executable code
         * for which basic-block data cannot be obtained.
         * An unknown node should not appear in a well-formed 
         * graph.
         * @return ID number of the unknown node.
         */
        virtual UINT32 get_unknown_node_id() const =0;

        /**
         * Get the ID of an edge given its source and target nodes.
         * @return ID number of edge or zero (0) if there is
         * no edge between the two nodes.
         */
        virtual DCFG_ID
        get_edge_id(DCFG_ID source_node_id,
                    /**< [in] ID number of node the edge is coming from. */
                    DCFG_ID target_node_id
                    /**< [in] ID number of node the edge is going to. */
                    ) const =0;

        /**
         * Get the set of target nodes that have an edge from the given source.
         * Successor node sets are used in various graph algorithms.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_successor_node_ids(DCFG_ID source_node_id,
                               /**< [in] ID number of source node. */
                               DCFG_ID_CONTAINER &node_ids
                              /**< [out] Container to which target node IDs are added.
                                 Previous contents of the container are *not* emptied
                                 by this call, so it should be emptied by the
                                 programmer before the call if desired.  The
                                 programmer can use any implementation of
                                 DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                 etc. */
                              ) const =0;


        /**
         * Get the set of source nodes that have an edge to the given target.
         * Predecessor node sets are used in various graph algorithms.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_predecessor_node_ids(DCFG_ID target_node_id,
                               /**< [in] ID number of target node. */
                               DCFG_ID_CONTAINER &node_ids
                              /**< [out] Container to which source node IDs are added.
                                 Previous contents of the container are *not* emptied
                                 by this call, so it should be emptied by the
                                 programmer before the call if desired.  The
                                 programmer can use any implementation of
                                 DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                 etc. */
                              ) const =0;

        /**
         * Get access to data for an edge.
         * @return Pointer to interface object for specified edge or `NULL`
         * if `edge_id` is invalid.
         */
        virtual DCFG_EDGE_CPTR
        get_edge_info(DCFG_ID edge_id
                             /**< [in] ID of desired edge. */
                             ) const =0;

        /**
         * Get access to data for a basic block.
         * @return Pointer to interface object for specified basic block or `NULL`
         * if `node_id` refers to a "special" node or is invalid.
         */
        virtual DCFG_BASIC_BLOCK_CPTR
        get_basic_block_info(DCFG_ID node_id
                             /**< [in] ID of desired basic block. */
                             ) const =0;

        /**
         * Determine whether a node ID refers to any "special"
         * (non-basic-block) node.
         * This could be a start, end, or unknown node.
         * If this returns `false` it does not necessarily
         * mean that the node is a basic-block; it could
         * be that the ID is invalid.
         * @return `true` if node is special, `false` otherwise.
         */
        virtual bool
        is_special_node(DCFG_ID node_id
                        /**< [in] ID of node in question. */
                        ) const =0;

        /**
         * Determine whether a node ID refers to the special
         * non-basic-block start node.
         * @return `true` if start node, `false` otherwise.
         */
        virtual bool
        is_start_node(DCFG_ID node_id
                      /**< [in] ID of node in question. */
                      ) const =0;
        
        /**
         * Determine whether a node ID refers to the special
         * non-basic-block end node.
         * @return `true` if end node, `false` otherwise.
         */
        virtual bool
        is_end_node(DCFG_ID node_id
                      /**< [in] ID of node in question. */
                      ) const =0;
        
        /**
         * Determine whether a node ID refers to the special
         * non-basic-block "unknown" node.
         * A well-formed DCFG should not have any unknown
         * nodes.
         * @return `true` if unknown node, `false` otherwise.
         */
        virtual bool
        is_unknown_node(DCFG_ID node_id
                        /**< [in] ID of node in question. */
                        ) const =0;
        

    };

    /**
     * Interface to information about a binary image within a process.
     */
    class DCFG_IMAGE :
        public virtual DCFG_ROUTINE_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_IMAGE() {}

        /**
         * Get the process ID.
         * @return Process ID of this image.
         */
        virtual DCFG_ID get_process_id() const =0;

        /**
         * Get the image ID.
         * @return ID of this image.
         */
        virtual DCFG_ID get_image_id() const =0;

        /**
         * Get the filename of the image.
         * @return Pointer to string containing full pathname of image or
         * base name if pathname not available or NULL if no name is available.
         */
        virtual const std::string* get_filename() const =0;

        /**
         * Get base address of image.
         * @return Address where image was loaded into memory by O/S.
         */
        virtual UINT64 get_base_address() const =0;

        /**
         * Get size of image.
         * @return Size of image as loaded into memory by O/S, in bytes.
         */
        virtual UINT64 get_size() const =0;

        /**
         * Get basic block ID(s) containing given address
         * in this image.
         * It is possible to get zero or more IDs returned:
         * zero if the address appears in no basic blocks,
         * one if it appears in exactly one block in one image,
         * and more than one if it is not unique.
         * Basic blocks may not be unique if an
         * image uses self-modifying code (SMC) or other
         * mechanisms that replace code regions. For most 
         * images, this will not be the case, and addresses
         * will be unique for a given image.
         * @return Number of IDs that were added to `node_ids`.
         */
        virtual UINT32
        get_basic_block_ids_by_addr(UINT64 addr,
                                   /**< [in] Virtual address that can appear
                                      anywhere within a basic block. */
                                   DCFG_ID_CONTAINER &node_ids
                                   /**< [out] Container to which basic-block IDs are added.
                                      Previous contents of the container are *not*
                                      emptied by this call, so it should be emptied by
                                      the programmer before the call if desired.  The
                                      programmer can use any implementation of
                                      DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                                      etc. */
                                    ) const =0;

    };

    /**
     * Interface to information about a routine in an image.
     * A routine is also known as a subroutine, function, or procedure.
     */
    class DCFG_ROUTINE :
        public virtual DCFG_LOOP_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_ROUTINE() {}

        /**
         * Get the process ID.
         * @return Process ID of this routine.
         */
        virtual DCFG_ID get_process_id() const =0;

        /**
         * Get the image ID.
         * @return Image ID of this routine.
         */
        virtual DCFG_ID get_image_id() const =0;

        /**
         * Get routine ID, which equals the basic-block ID of the entry node.
         * By the DCFG definition, a routine can only
         * have one entry node.  If there is a call into the "middle" of a
         * routine, that entry point defines a separate routine in a DCFG.
         * @return ID number of entry node.
         */
        virtual DCFG_ID get_routine_id() const =0;

        /**
         * Get symbol name of this routine.
         * For more comprehensive symbol and source-code data,
         * use DCFG_BASIC_BLOCK::get_symbol_name(),
         * DCFG_BASIC_BLOCK::get_source_filename(),
         * and DCFG_BASIC_BLOCK::get_source_line_number()
         * for one or more basic blocks in this routine.
         * @return Pointer to name of the symbol at the entry node of the
         * routine if it exists, `NULL` otherwise.
         */
        virtual const std::string* get_symbol_name() const =0;

        /**
         * Get set of entry edge IDs.
         * For routines, these are typically from "call" statements, but
         * they can also include branches to routines for unstructured
         * code.
         * The source of an entry edge will be from another
         * routine except in the case of direct recursion (call
         * from routine to itself).
         * The target of an entry edge will always be the entry node.
         * This set does *not* include incoming return edges.
         * If you also want return edges, use
         * DCFG_GRAPH_BASE::get_inbound_edge_ids().
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_entry_edge_ids(DCFG_ID_CONTAINER &edge_ids
                          /**< [out] Container to which exit edge IDs are added.
                             Previous contents of the container are *not*
                             emptied by this call, so it should be emptied by
                             the programmer before the call if desired.  The
                             programmer can use any implementation of
                             DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                             etc. */
                          ) const =0;

        /**
         * Get set of exit edge IDs.
         * For routines, these are typically from "return" statements, but
         * they can also include branches out of routines for unstructured
         * code.
         * The target of an exit edge will be to another
         * routine except for direct recursion.
         * This set does *not* include outgoing call edges.
         * If you also want call edges, use
         * DCFG_GRAPH_BASE::get_outbound_edge_ids().
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_exit_edge_ids(DCFG_ID_CONTAINER &edge_ids
                          /**< [out] Container to which exit edge IDs are added.
                             Previous contents of the container are *not*
                             emptied by this call, so it should be emptied by
                             the programmer before the call if desired.  The
                             programmer can use any implementation of
                             DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                             etc. */
                          ) const =0;

        /**
         * Get immediate dominator.
         * The immediate dominator (idom) is the last node before the
         * given node that must be executed before the given node
         * is executed.
         * The idom of the entry node is itself.
         * The idom must be within the routine, i.e., 
         * it does not consider edges between routines.
         * Idoms relationships are used in many graph algorithms.
         * @return ID number of idom of `node_id` or zero (0)
         * if `node_id` is not in this routine.
         */
        virtual DCFG_ID
        get_idom_node_id(DCFG_ID node_id
                         /**< [in] ID number of *dominated* node. */
                         ) const =0;

        /**
         * Get dynamic entry count.  This is the number of times the routine
         * was called or otherwise entered.  By the DCFG definition, a
         * routine can only be entered at its entry node.  A call within a
         * routine to its entry node is considered an entry (via recursion).
         * If there is a branch within a routine to its entry node, it will
         * also be considered an entry (this is unusual).
         * @return Number of times routine was entered,
         * summed across all threads.
         */
        virtual UINT64 get_entry_count() const =0;

        /**
         * Get dynamic entry count per thread.
         * See DCFG_ROUTINE::get_entry_count() for entry-count
         * definition.
         * @return Number of times routine
         * was entered in given thread.
         */
        virtual UINT64
        get_entry_count_for_thread(UINT32 thread_id
                                   /**< [in] Thread number.  Typically, threads
                                    * are consecutively numbered from zero to
                                    * DCFG_PROCESS::get_highest_thread_id(). */
                                   ) const =0;

    };

    /**
     * Interface to information about a loop.
     */
    class DCFG_LOOP :
        public virtual DCFG_GRAPH_BASE {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_LOOP() {}

        /**
         * Get the process ID.
         * @return Process ID of this loop.
         */
        virtual DCFG_ID get_process_id() const =0;

        /**
         * Get the image ID.
         * @return Image ID of this loop.
         */
        virtual DCFG_ID get_image_id() const =0;

        /**
         * Get routine ID.
         * @return routine ID number of this loop.
         */
        virtual DCFG_ID get_routine_id() const =0;

        /**
         * Get loop ID, which equals the basic-block ID of the head node.
         * The head node is the common target of all the back edges in
         * the loop.
         * @return ID number of head node.
         */
        virtual DCFG_ID get_loop_id() const =0;

        /**
         * Get set of IDs of the entry edges.  
         * These are the edges that are traversed when a loop is entered
         * from somewhere outside the loop.
         * This set does *not* include back edges.
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_entry_edge_ids(DCFG_ID_CONTAINER &edge_ids
                          /**< [out] Container to which edge IDs are added.
                             Previous contents of the container are *not*
                             emptied by this call, so it should be emptied by
                             the programmer before the call if desired.  The
                             programmer can use any implementation of
                             DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                             etc. */
                          ) const =0;

        /**
         * Get set of IDs of the exit edges.  
         * These are the edges that are traversed when a loop is exited.
         * This set does *not* include call edges from the loop.
         * If you also want call edges, use
         * DCFG_GRAPH_BASE::get_outbound_edge_ids().
         * Note that any given edge may exit more than one loop when
         * loops are nested.
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_exit_edge_ids(DCFG_ID_CONTAINER &edge_ids
                          /**< [out] Container to which edge IDs are added.
                             Previous contents of the container are *not*
                             emptied by this call, so it should be emptied by
                             the programmer before the call if desired.  The
                             programmer can use any implementation of
                             DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                             etc. */
                          ) const =0;

        /**
         * Get set of IDs of the back-edges.  These are the edges that are
         * traversed when a loop is repeated following an entry.
         * The target node of each back edge will be the head node of the
         * loop by definition.  By definition, an edge n->h is a back edge
         * if h dominates n, where h is the head node.
         * @return Number of IDs that were added to `edge_ids`.
         */
        virtual UINT32
        get_back_edge_ids(DCFG_ID_CONTAINER &edge_ids
                          /**< [out] Container to which edge IDs are added.
                             Previous contents of the container are *not*
                             emptied by this call, so it should be emptied by
                             the programmer before the call if desired.  The
                             programmer can use any implementation of
                             DCFG_ID_CONTAINER: DCFG_ID_VECTOR, DCFG_ID_SET,
                             etc. */
                          ) const =0;


        /**
         * Get head node ID of most immediate containing loop, if any.
         * This indicates loop nesting.
         * @return ID number of head node of parent loop or
         * zero (0) if there is no parent loop.
         */

        virtual DCFG_ID get_parent_loop_id() const =0;


        /**
         * Get dynamic entry count.  This is the number of times the loop
         * was entered, which will be less than or equal to 
         * get_iteration_count(). A loop can only be entered at
         * its head node.
         * @return Number of times loop was entered,
         * summed across all threads.
         */
        virtual UINT64 get_entry_count() const =0;

        /**
         * Get dynamic entry count per thread.  This is the number of times
         * the loop was entered, which will be less than or equal to
         * get_iteration_count(). A loop can only be entered at its head
         * node.
         * @return Number of times loop was entered in given thread.
         */
        virtual UINT64
        get_entry_count_for_thread(UINT32 thread_id
                                   /**< [in] Thread number.  Typically, threads
                                    * are consecutively numbered from zero to
                                    * DCFG_PROCESS::get_highest_thread_id(). */
                                   ) const =0;


        /**
         * Get dynamic iteration count.
         * This is the number of times the loop was executed,
         * including entry from outside the loop and via
         * its back edges.
         * By definition, a loop can only be entered at its
         * head node.
         * @return Number of times loop was execcuted,
         * summed across all threads.
         */
        virtual UINT64 get_iteration_count() const =0;

        /**
         * Get dynamic execution count per thread.
         * See DCFG_LOOP::get_iteration_count() for iteration-count
         * definition.
         * @return Number of times loop was executed
         * in given thread.
         */
        virtual UINT64
        get_iteration_count_for_thread(UINT32 thread_id
                                       /**< [in] Thread number.  Typically, threads
                                        * are consecutively numbered from zero to
                                        * DCFG_PROCESS::get_highest_thread_id(). */
                                       ) const =0;

    };

    /**
     * Interface to information about a basic block.
     */
    class DCFG_BASIC_BLOCK :
        public virtual DCFG_GRAPH_BASE {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_BASIC_BLOCK() {}

        /**
         * Get basic-block ID number.
         * Basic-block ID numbers are unique within a process.
         * @return ID number of this basic block.
         */
        virtual DCFG_ID get_basic_block_id() const =0;

        /**
         * Get the process ID.
         * @return Process ID of this block.
         */
        virtual DCFG_ID get_process_id() const =0;

        /**
         * Get the image ID.
         * @return Image ID of this block.
         */
        virtual DCFG_ID get_image_id() const =0;

        /**
         * Get routine ID.
         * @return routine ID number of this block or 
         * zero (0) if none.
         */
        virtual DCFG_ID get_routine_id() const =0;

        /**
         * Get innermost loop ID.
         * To find all loops containing this block, get the
         * innermost loop and then follow the parent loop IDs until
         * there are no parents.
         * @return ID number of innermost loop containing this block or 
         * zero (0) if none.
         */
        virtual DCFG_ID get_inner_loop_id() const =0;

        /**
         * Get starting or base address.
         * @return Address of first instruction in this block.
         */
        virtual UINT64 get_first_instr_addr() const =0;

        /**
         * Get the address of the last instruction.
         * This is *not* the address of the last byte in the block
         * unless the last instruction is exactly one byte long.
         * The address of the last byte is
         * DCFG_BASIC_BLOCK::get_first_instr_addr() +
         * DCFG_BASIC_BLOCK::get_size() - 1.
         * @return Address of last instruction in this block.
         */
        virtual UINT64 get_last_instr_addr() const =0;

        /**
         * Get size.
         * @return Size of this block in bytes.
         */
        virtual UINT32 get_size() const =0;

        /**
         * Get the raw machine-code bytes, if available.
         * @return Pointer to get_size() bytes or `NULL`
         * if not available.
         */
        virtual const UINT8* get_raw_bytes() const =0;

        /**
         * Get *static* number of instructions in the block.
         * To get the dynamic count of instructions executed,
         * use DCFG_GRAPH_BASE::get_instr_count() or
         * DCFG_GRAPH_BASE::get_instr_count_for_thread().
         * @return Static number of instructions in this block.
         */
        virtual UINT32 get_num_instrs() const =0;

        /**
         * Get symbol name of this block.
         * @return Pointer to name of the symbol at the base address
         * of this block if one exists, `NULL` otherwise.
         */
        virtual const std::string* get_symbol_name() const =0;

        /**
         * Get symbol offset of this block.
         * @return Difference between base address of the symbol
         * returned in DCFG_BASIC_BLOCK::get_symbol_name()
         * and the base address of this block or zero (0) if
         * no symbol exits.
         */
        virtual UINT32 get_symbol_offset() const =0;

        /**
         * Get name of source file for this block.
         * @return Pointer to name of the source filename at the base address
         * of this block if it exists, `NULL` otherwise.
         */
        virtual const std::string* get_source_filename() const =0;

        /**
         * Get line number in source file for this block.
         * @return Line number at the base address
         * of this block if it exists, zero (0) otherwise.
         */
        virtual UINT32 get_source_line_number() const =0;

        /**
         * Get dynamic execution count.
         * @return Number of times the block was executed,
         * summed across all threads.
         */
        virtual UINT64 get_exec_count() const =0;

        /**
         * Get dynamic execution count.
         * @return Number of times the block was executed
         * in given thread.
         */
        virtual UINT64
        get_exec_count_for_thread(UINT32 thread_id
                                  /**< [in] Thread number.  Typically, threads
                                   * are consecutively numbered from zero to
                                   * DCFG_PROCESS::get_highest_thread_id(). */
                                  ) const =0;

    };

    /**
     * Interface to information about an edge between basic blocks
     * and/or special nodes.
     */
    class DCFG_EDGE {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_EDGE() {}

        /**
         * Get ID number of edge.
         * @return ID number for this edge, unique within a process.
         */
        virtual DCFG_ID get_edge_id() const =0;

        /**
         * Get node ID of edge source.
         * This is the node node the edge is "coming from".
         * Most node IDs correspond to basic-blocks,
         * but they could also be for special nodes. In particular, a source
         * node ID could be the "START" node.
         * @return ID of source node.
         */
        virtual DCFG_ID get_source_node_id() const =0;

        /**
         * Get node ID of edge target.
         * This is the node node the edge is "going to".
         * Most node IDs correspond to basic-blocks,
         * but they could also be for special nodes. In particular, a source
         * node ID could be the "START" node.
         * @return ID of target node.
         */
        virtual DCFG_ID get_target_node_id() const =0;

        /**
         * Get edge count.
         * @return Number of times edge was taken, summed across all threads.
         */
        virtual UINT64 get_exec_count() const =0;

        /**
         * Get edge count per thread.
         * @return Number of times edge was taken on given thread.
         */
        virtual UINT64
        get_exec_count_for_thread(UINT32 thread_id
                             /**< [in] Thread number.  Typically, threads
                              * are consecutively numbered from zero to
                              * DCFG_PROCESS::get_highest_thread_id(). */
                             ) const =0;

        /**
         * Get edge type.
         * @return Pointer to string describing edge type per DCFG format
         * documentation or NULL if type data is internally inconsistent
         * (should not happen).
         */
        virtual const std::string* get_edge_type() const =0;

        /**
         * Determine whether this edge is *any* type of branch.
         * @return `true` if branch, `false` otherwise.
         */
        virtual bool is_any_branch_type() const =0;

        /**
         * Determine whether this edge is *any* type of call.
         * This includes routine calls, system calls, etc.
         * @return `true` if call, `false` otherwise.
         */
        virtual bool is_any_call_type() const =0;

        /**
         * Determine whether this edge is *any* type of return.
         * This includes routine returns, system returns, etc.
         * @return `true` if return, `false` otherwise.
         */
        virtual bool is_any_return_type() const =0;

        /**
         * Determine whether this edge is *any* type of call or
         * return *or* an edge from the start node or to
         * the exit node.
         * @return `true` if inter-routine, `false` otherwise.
         */
        virtual bool is_any_inter_routine_type() const =0;

        /**
         * Determine whether this edge is *any* type of bypass,
         * which is a "fabricated" edge across call/return
         * pairs, etc. See the DCFG documentation for more
         * information on bypasses.
         * @return `true` if bypass, `false` otherwise.
         */
        virtual bool is_any_bypass_type() const =0;

        /**
         * Determine whether this edge is a
         * branch edge type.
         * @return `true` if branch edge,
         * `false` otherwise.
         */
        virtual bool is_branch_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * call edge type.
         * @return `true` if call edge,
         * `false` otherwise.
         */
        virtual bool is_call_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * return edge type.
         * @return `true` if return edge,
         * `false` otherwise.
         */
        virtual bool is_return_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * call bypass edge type.
         * @return `true` if call bypass edge,
         * `false` otherwise.
         */
        virtual bool is_call_bypass_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * conditional branch edge type.
         * @return `true` if conditional branch edge,
         * `false` otherwise.
         */
        virtual bool is_conditional_branch_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * context bypass edge type.
         * @return `true` if context bypass edge,
         * `false` otherwise.
         */
        virtual bool is_context_bypass_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * context edge type.
         * @return `true` if context edge,
         * `false` otherwise.
         */
        virtual bool is_context_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * context return edge type.
         * @return `true` if context return edge,
         * `false` otherwise.
         */
        virtual bool is_context_return_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * direct branch edge type.
         * @return `true` if direct branch edge,
         * `false` otherwise.
         */
        virtual bool is_direct_branch_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * direct call edge type.
         * @return `true` if direct call edge,
         * `false` otherwise.
         */
        virtual bool is_direct_call_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * direct conditional branch edge type.
         * @return `true` if direct conditional branch edge,
         * `false` otherwise.
         */
        virtual bool is_direct_conditional_branch_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * direct unconditional branch edge type.
         * @return `true` if direct unconditional branch edge,
         * `false` otherwise.
         */
        virtual bool is_direct_unconditional_branch_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * entry edge type.
         * @return `true` if entry edge,
         * `false` otherwise.
         */
        virtual bool is_entry_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * excluded bypass edge type.
         * @return `true` if excluded bypass edge,
         * `false` otherwise.
         */
        virtual bool is_excluded_bypass_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * exit edge type.
         * @return `true` if exit edge,
         * `false` otherwise.
         */
        virtual bool is_exit_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * fall thru edge type.
         * @return `true` if fall thru edge,
         * `false` otherwise.
         */
        virtual bool is_fall_thru_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * indirect branch edge type.
         * @return `true` if indirect branch edge,
         * `false` otherwise.
         */
        virtual bool is_indirect_branch_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * indirect call edge type.
         * @return `true` if indirect call edge,
         * `false` otherwise.
         */
        virtual bool is_indirect_call_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * indirect conditional branch edge type.
         * @return `true` if indirect conditional branch edge,
         * `false` otherwise.
         */
        virtual bool is_indirect_conditional_branch_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * indirect unconditional branch edge type.
         * @return `true` if indirect unconditional branch edge,
         * `false` otherwise.
         */
        virtual bool is_indirect_unconditional_branch_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * rep-prefix edge type.
         * @return `true` if rep edge,
         * `false` otherwise.
         */
        virtual bool is_rep_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * system call bypass edge type.
         * @return `true` if sys call bypass edge,
         * `false` otherwise.
         */
        virtual bool is_sys_call_bypass_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * system call edge type.
         * @return `true` if sys call edge,
         * `false` otherwise.
         */
        virtual bool is_sys_call_edge_type() const =0;

        /**
         * Determine whether this edge is a
         * system return edge type.
         * @return `true` if sys return edge,
         * `false` otherwise.
         */
        virtual bool is_sys_return_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * unconditional branch edge type.
         * @return `true` if unconditional branch edge,
         * `false` otherwise.
         */
        virtual bool is_unconditional_branch_edge_type() const =0;

        /**
         * Determine whether this edge is an
         * unknown edge type.
         * @return `true` if unknown edge,
         * `false` otherwise.
         */
        virtual bool is_unknown_edge_type() const =0;
    };


    /**
     * Interface for any container of ID numbers.  The API programmer is
     * free to define and use any class that implements this interface to
     * collect ID numbers (process IDs, basic blocks, etc.) from the
     * relevant DCFG APIs.
     */
    class DCFG_ID_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_ID_CONTAINER() {}

        /**
         * Add one ID to the container.
         * All concrete implementations must define this method.
         */
        virtual void add_id(DCFG_ID id
                            /**< [in] ID to add. */
                            )=0;
    };

    /**
     * Vector of ID numbers.
     * This is an example of a DCFG_ID_CONTAINER implementation
     * based on an STL `vector`. This is useful when it is 
     * important to maintain the order in which elements are
     * added.
     */
    class DCFG_ID_VECTOR :
        public std::vector<DCFG_ID>,
        public DCFG_ID_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_ID_VECTOR() {}

        virtual void add_id(DCFG_ID id) {
            push_back(id);
        }
    };

    /**
     * Set of ID numbers.  This is an example of a DCFG_ID_CONTAINER
     * implementation based on an STL `set`. This is useful for iterating
     * through the added IDs in numerical order and checking whether a
     * certain ID exists.
     */
    class DCFG_ID_SET :
        public std::set<DCFG_ID>,
        public DCFG_ID_CONTAINER {
    public:

        // Default virtual destructor to ensure cleanup.
        virtual ~DCFG_ID_SET() {}

        virtual void add_id(DCFG_ID id) {
            insert(id);
        }
    };

};                           // namespace
#endif
