// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
//
// Copyright 2009-2020 by Wilson Snyder. This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//=========================================================================
///
/// \file
/// \brief Verilator: Implementation Header, only for verilated.cpp internals.
///
/// Code available from: https://verilator.org
///
//=========================================================================

#ifndef _VERILATED_IMP_H_
#define _VERILATED_IMP_H_ 1  ///< Header Guard

// clang-format off
#if !defined(_VERILATED_CPP_) && !defined(_VERILATED_DPI_CPP_) && !defined(_VERILATED_VPI_CPP_)
# error "verilated_imp.h only to be included by verilated*.cpp internals"
#endif

#include "verilatedos.h"
#include "verilated.h"
#include "verilated_heavy.h"
#include "verilated_syms.h"

#include <deque>
#include <set>
#include <vector>
#include <numeric>
#ifdef VL_THREADED
# include <functional>
# include <queue>
#endif
// clang-format on

class VerilatedScope;

//======================================================================
// Threaded message passing

#ifdef VL_THREADED
/// Message, enqueued on an mtask, and consumed on the main eval thread
class VerilatedMsg {
public:
    // TYPES
    struct Cmp {
        bool operator()(const VerilatedMsg& a, const VerilatedMsg& b) const {
            return a.mtaskId() < b.mtaskId();
        }
    };

private:
    // MEMBERS
    vluint32_t m_mtaskId;  ///< MTask that did enqueue
    std::function<void()> m_cb;  ///< Lambda to execute when message received
public:
    // CONSTRUCTORS
    VerilatedMsg(const std::function<void()>& cb)
        : m_mtaskId(Verilated::mtaskId())
        , m_cb(cb) {}
    ~VerilatedMsg() {}
    // METHODS
    vluint32_t mtaskId() const { return m_mtaskId; }
    /// Execute the lambda function
    void run() const { m_cb(); }
};

/// Each thread has a queue it pushes to
/// This assumes no thread starts pushing the next tick until the previous has drained.
/// If more aggressiveness is needed, a double-buffered scheme might work well.
class VerilatedEvalMsgQueue {
    typedef std::multiset<VerilatedMsg, VerilatedMsg::Cmp> VerilatedThreadQueue;

    std::atomic<vluint64_t> m_depth;  ///< Current depth of queue (see comments below)

    VerilatedMutex m_mutex;  ///< Mutex protecting queue
    VerilatedThreadQueue m_queue VL_GUARDED_BY(m_mutex);  ///< Message queue
public:
    // CONSTRUCTORS
    VerilatedEvalMsgQueue()
        : m_depth(0) {
        assert(atomic_is_lock_free(&m_depth));
    }
    ~VerilatedEvalMsgQueue() {}

private:
    VL_UNCOPYABLE(VerilatedEvalMsgQueue);

public:
    // METHODS
    //// Add message to queue (called by producer)
    void post(const VerilatedMsg& msg) VL_EXCLUDES(m_mutex) {
        VerilatedLockGuard lock(m_mutex);
        m_queue.insert(msg);  // Pass by value to copy the message into queue
        ++m_depth;
    }
    /// Service queue until completion (called by consumer)
    void process() VL_EXCLUDES(m_mutex) {
        // Tracking m_depth is redundant to e.g. getting the mutex and looking at queue size,
        // but on the reader side it's 4x faster to test an atomic then getting a mutex
        while (m_depth) {
            // Wait for a message to be added to the queue
            // We don't use unique_lock as want to unlock with the message copy still in scope
            m_mutex.lock();
            assert(!m_queue.empty());  // Otherwise m_depth is wrong
            // Unfortunately to release the lock we need to copy the message
            // (Or have the message be a pointer, but then new/delete cost on each message)
            // We assume messages are small, so copy
            auto it = m_queue.begin();
            const VerilatedMsg msg = *(it);
            m_queue.erase(it);
            m_mutex.unlock();
            m_depth--;  // Ok if outside critical section as only this code checks the value
            {
                VL_DEBUG_IF(VL_DBG_MSGF("Executing callback from mtaskId=%d\n", msg.mtaskId()););
                msg.run();
            }
        }
    }
};

/// Each thread has a local queue to build up messages until the end of the eval() call
class VerilatedThreadMsgQueue {
    std::queue<VerilatedMsg> m_queue;

public:
    // CONSTRUCTORS
    VerilatedThreadMsgQueue() {}
    ~VerilatedThreadMsgQueue() {
        // The only call of this with a non-empty queue is a fatal error.
        // So this does not flush the queue, as the destination queue is not known to this class.
    }

private:
    VL_UNCOPYABLE(VerilatedThreadMsgQueue);
    // METHODS
    static VerilatedThreadMsgQueue& threadton() {
        static VL_THREAD_LOCAL VerilatedThreadMsgQueue t_s;
        return t_s;
    }

public:
    /// Add message to queue, called by producer
    static void post(const VerilatedMsg& msg) VL_MT_SAFE {
        // Handle calls to threaded routines outside
        // of any mtask -- if an initial block calls $finish, say.
        if (Verilated::mtaskId() == 0) {
            // No queueing, just do the action immediately
            msg.run();
        } else {
            Verilated::endOfEvalReqdInc();
            threadton().m_queue.push(msg);  // Pass by value to copy the message into queue
        }
    }
    /// Push all messages to the eval's queue
    static void flush(VerilatedEvalMsgQueue* evalMsgQp) VL_MT_SAFE {
        while (!threadton().m_queue.empty()) {
            evalMsgQp->post(threadton().m_queue.front());
            threadton().m_queue.pop();
            Verilated::endOfEvalReqdDec();
        }
    }
};
#endif  // VL_THREADED

//======================================================================
// VerilatedImp

class VerilatedImp {
    // Whole class is internal use only - Global information shared between verilated*.cpp files.
protected:
    friend class Verilated;

    // TYPES
    typedef std::vector<std::string> ArgVec;
    typedef std::map<std::pair<const void*, void*>, void*> UserMap;
    typedef std::map<const char*, int, VerilatedCStrCmp> ExportNameMap;

    // MEMBERS
    static VerilatedImp s_s;  ///< Static Singleton; One and only static this

    struct Serialized {  // All these members serialized/deserialized
        int m_timeFormatUnits;  // $timeformat units
        int m_timeFormatPrecision;  // $timeformat number of decimal places
        int m_timeFormatWidth;  // $timeformat character width
        enum { UNITS_NONE = 99 };  // Default based on precision
        Serialized()
            : m_timeFormatUnits(UNITS_NONE)
            , m_timeFormatPrecision(0)
            , m_timeFormatWidth(20) {}
        ~Serialized() {}
    } m_ser;

    VerilatedMutex m_sergMutex;  ///< Protect m_ser
    struct SerializedG {  // All these members serialized/deserialized and guarded
        std::string m_timeFormatSuffix;  // $timeformat printf format
    } m_serg VL_GUARDED_BY(m_sergMutex);

    // Nothing below here is save-restored; users expected to re-register appropriately

    VerilatedMutex m_argMutex;  ///< Protect m_argVec, m_argVecLoaded
    /// Argument list (NOT save-restored, may want different results)
    ArgVec m_argVec VL_GUARDED_BY(m_argMutex);
    bool m_argVecLoaded VL_GUARDED_BY(m_argMutex);  ///< Ever loaded argument list

    VerilatedMutex m_userMapMutex;  ///< Protect m_userMap
    UserMap m_userMap VL_GUARDED_BY(m_userMapMutex);  ///< Map of <(scope,userkey), userData>

    VerilatedMutex m_nameMutex;  ///< Protect m_nameMap
    /// Map of <scope_name, scope pointer>
    VerilatedScopeNameMap m_nameMap VL_GUARDED_BY(m_nameMutex);

    VerilatedMutex m_hierMapMutex;  ///< Protect m_hierMap
    /// Map the represents scope hierarchy
    VerilatedHierarchyMap m_hierMap VL_GUARDED_BY(m_hierMapMutex);

    // Slow - somewhat static:
    VerilatedMutex m_exportMutex;  ///< Protect m_nameMap
    /// Map of <export_func_proto, func number>
    ExportNameMap m_exportMap VL_GUARDED_BY(m_exportMutex);
    int m_exportNext VL_GUARDED_BY(m_exportMutex);  ///< Next export funcnum

    // File I/O
    VerilatedMutex m_fdMutex;  ///< Protect m_fdps, m_fdFree
    std::vector<FILE*> m_fdps VL_GUARDED_BY(m_fdMutex);  ///< File descriptors
    /// List of free descriptors (SLOW - FOPEN/CLOSE only)
    std::vector<IData> m_fdFree VL_GUARDED_BY(m_fdMutex);
    // List of free descriptors in the MCT region [4, 32)
    std::vector<IData> m_fdFreeMct VL_GUARDED_BY(m_fdMutex);

public:  // But only for verilated*.cpp
    // CONSTRUCTORS
    VerilatedImp()
        : m_argVecLoaded(false)
        , m_exportNext(0) {
        s_s.m_fdps.resize(31);
        std::fill(s_s.m_fdps.begin(), s_s.m_fdps.end(), (FILE*)0);
        s_s.m_fdFreeMct.resize(30);
        for (int i = 0, id = 1; i < s_s.m_fdFreeMct.size(); i++, id++) { s_s.m_fdFreeMct[i] = id; }
    }
    ~VerilatedImp() {}

private:
    VL_UNCOPYABLE(VerilatedImp);

public:
    // METHODS - debug
    static void internalsDump() VL_MT_SAFE;
    static void versionDump() VL_MT_SAFE;

    // METHODS - arguments
public:
    static void commandArgs(int argc, const char** argv) VL_EXCLUDES(s_s.m_argMutex);
    static void commandArgsAdd(int argc, const char** argv) VL_EXCLUDES(s_s.m_argMutex);
    static std::string argPlusMatch(const char* prefixp) VL_EXCLUDES(s_s.m_argMutex) {
        VerilatedLockGuard lock(s_s.m_argMutex);
        // Note prefixp does not include the leading "+"
        size_t len = strlen(prefixp);
        if (VL_UNLIKELY(!s_s.m_argVecLoaded)) {
            s_s.m_argVecLoaded = true;  // Complain only once
            VL_FATAL_MT("unknown", 0, "",
                        "%Error: Verilog called $test$plusargs or $value$plusargs without"
                        " testbench C first calling Verilated::commandArgs(argc,argv).");
        }
        for (ArgVec::const_iterator it = s_s.m_argVec.begin(); it != s_s.m_argVec.end(); ++it) {
            if ((*it)[0] == '+') {
                if (0 == strncmp(prefixp, it->c_str() + 1, len)) return *it;
            }
        }
        return "";
    }

private:
    static void commandArgsAddGuts(int argc, const char** argv) VL_REQUIRES(s_s.m_argMutex);
    static void commandArgVl(const std::string& arg);
    static bool commandArgVlValue(const std::string& arg, const std::string& prefix,
                                  std::string& valuer);

public:
    // METHODS - user scope tracking
    // We implement this as a single large map instead of one map per scope
    // There's often many more scopes than userdata's and thus having a ~48byte
    // per map overhead * N scopes would take much more space and cache thrashing.
    static inline void userInsert(const void* scopep, void* userKey, void* userData) VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_userMapMutex);
        UserMap::iterator it = s_s.m_userMap.find(std::make_pair(scopep, userKey));
        if (it != s_s.m_userMap.end()) {
            it->second = userData;
        } else {
            s_s.m_userMap.insert(it, std::make_pair(std::make_pair(scopep, userKey), userData));
        }
    }
    static inline void* userFind(const void* scopep, void* userKey) VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_userMapMutex);
        UserMap::const_iterator it = s_s.m_userMap.find(std::make_pair(scopep, userKey));
        if (VL_UNLIKELY(it == s_s.m_userMap.end())) return NULL;
        return it->second;
    }

private:
    /// Symbol table destruction cleans up the entries for each scope.
    static void userEraseScope(const VerilatedScope* scopep) VL_MT_SAFE {
        // Slow ok - called once/scope on destruction, so we simply iterate.
        VerilatedLockGuard lock(s_s.m_userMapMutex);
        for (UserMap::iterator it = s_s.m_userMap.begin(); it != s_s.m_userMap.end();) {
            if (it->first.first == scopep) {
                s_s.m_userMap.erase(it++);
            } else {
                ++it;
            }
        }
    }
    static void userDump() VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_userMapMutex);  // Avoid it changing in middle of dump
        bool first = true;
        for (UserMap::const_iterator it = s_s.m_userMap.begin(); it != s_s.m_userMap.end(); ++it) {
            if (first) {
                VL_PRINTF_MT("  userDump:\n");
                first = false;
            }
            VL_PRINTF_MT("    DPI_USER_DATA scope %p key %p: %p\n", it->first.first,
                         it->first.second, it->second);
        }
    }

public:  // But only for verilated*.cpp
    // METHODS - scope name
    static void scopeInsert(const VerilatedScope* scopep) VL_MT_SAFE {
        // Slow ok - called once/scope at construction
        VerilatedLockGuard lock(s_s.m_nameMutex);
        VerilatedScopeNameMap::iterator it = s_s.m_nameMap.find(scopep->name());
        if (it == s_s.m_nameMap.end()) {
            s_s.m_nameMap.insert(it, std::make_pair(scopep->name(), scopep));
        }
    }
    static inline const VerilatedScope* scopeFind(const char* namep) VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_nameMutex);
        // If too slow, can assume this is only VL_MT_SAFE_POSINIT
        VerilatedScopeNameMap::const_iterator it = s_s.m_nameMap.find(namep);
        if (VL_UNLIKELY(it == s_s.m_nameMap.end())) return NULL;
        return it->second;
    }
    static void scopeErase(const VerilatedScope* scopep) VL_MT_SAFE {
        // Slow ok - called once/scope at destruction
        VerilatedLockGuard lock(s_s.m_nameMutex);
        userEraseScope(scopep);
        VerilatedScopeNameMap::iterator it = s_s.m_nameMap.find(scopep->name());
        if (it != s_s.m_nameMap.end()) s_s.m_nameMap.erase(it);
    }
    static void scopesDump() VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_nameMutex);
        VL_PRINTF_MT("  scopesDump:\n");
        for (VerilatedScopeNameMap::const_iterator it = s_s.m_nameMap.begin();
             it != s_s.m_nameMap.end(); ++it) {
            const VerilatedScope* scopep = it->second;
            scopep->scopeDump();
        }
        VL_PRINTF_MT("\n");
    }
    static const VerilatedScopeNameMap* scopeNameMap() VL_MT_SAFE_POSTINIT {
        // Thread save only assuming this is called only after model construction completed
        return &s_s.m_nameMap;
    }

public:  // But only for verilated*.cpp
    // METHODS - hierarchy
    static void hierarchyAdd(const VerilatedScope* fromp, const VerilatedScope* top) VL_MT_SAFE {
        // Slow ok - called at construction for VPI accessible elements
        VerilatedLockGuard lock(s_s.m_hierMapMutex);
        s_s.m_hierMap[fromp].push_back(top);
    }
    static const VerilatedHierarchyMap* hierarchyMap() VL_MT_SAFE_POSTINIT {
        // Thread save only assuming this is called only after model construction completed
        return &s_s.m_hierMap;
    }

public:  // But only for verilated*.cpp
    // METHODS - export names

    // Each function prototype is converted to a function number which we
    // then use to index a 2D table also indexed by scope number, because we
    // can't know at Verilation time what scopes will exist in other modules
    // in the design that also happen to have our same callback function.
    // Rather than a 2D map, the integer scheme saves 500ish ns on a likely
    // miss at the cost of a multiply, and all lookups move to slowpath.
    static int exportInsert(const char* namep) VL_MT_SAFE {
        // Slow ok - called once/function at creation
        VerilatedLockGuard lock(s_s.m_exportMutex);
        ExportNameMap::iterator it = s_s.m_exportMap.find(namep);
        if (it == s_s.m_exportMap.end()) {
            s_s.m_exportMap.insert(it, std::make_pair(namep, s_s.m_exportNext++));
            return s_s.m_exportNext++;
        } else {
            return it->second;
        }
    }
    static int exportFind(const char* namep) VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_exportMutex);
        ExportNameMap::const_iterator it = s_s.m_exportMap.find(namep);
        if (VL_LIKELY(it != s_s.m_exportMap.end())) return it->second;
        std::string msg = (std::string("%Error: Testbench C called ") + namep
                           + " but no such DPI export function name exists in ANY model");
        VL_FATAL_MT("unknown", 0, "", msg.c_str());
        return -1;
    }
    static const char* exportName(int funcnum) VL_MT_SAFE {
        // Slowpath; find name for given export; errors only so no map to reverse-map it
        VerilatedLockGuard lock(s_s.m_exportMutex);
        for (ExportNameMap::const_iterator it = s_s.m_exportMap.begin();
             it != s_s.m_exportMap.end(); ++it) {
            if (it->second == funcnum) return it->first;
        }
        return "*UNKNOWN*";
    }
    static void exportsDump() VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_exportMutex);
        bool first = true;
        for (ExportNameMap::const_iterator it = s_s.m_exportMap.begin();
             it != s_s.m_exportMap.end(); ++it) {
            if (first) {
                VL_PRINTF_MT("  exportDump:\n");
                first = false;
            }
            VL_PRINTF_MT("    DPI_EXPORT_NAME %05d: %s\n", it->second, it->first);
        }
    }
    // We don't free up m_exportMap until the end, because we can't be sure
    // what other models are using the assigned funcnum's.

public:  // But only for verilated*.cpp
    // METHODS - timeformat
    static std::string timeFormatSuffix() VL_MT_SAFE;
    static void timeFormatSuffix(const std::string& value) VL_MT_SAFE;
    static int timeFormatUnits() VL_MT_SAFE {
        if (s_s.m_ser.m_timeFormatUnits == Serialized::UNITS_NONE) {
            return Verilated::timeprecision();
        }
        return s_s.m_ser.m_timeFormatUnits;
    }
    static int timeFormatPrecision() VL_MT_SAFE { return s_s.m_ser.m_timeFormatPrecision; }
    static int timeFormatWidth() VL_MT_SAFE { return s_s.m_ser.m_timeFormatWidth; }
    static void timeFormatUnits(int value) VL_MT_SAFE;
    static void timeFormatPrecision(int value) VL_MT_SAFE;
    static void timeFormatWidth(int value) VL_MT_SAFE;

public:  // But only for verilated*.cpp
    // METHODS - file IO
    static IData fdNewMcd(const char* filenamep) VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_fdMutex);
        if (s_s.m_fdFreeMct.empty()) return 0;
        IData idx = s_s.m_fdFreeMct.back();
        s_s.m_fdFreeMct.pop_back();
        s_s.m_fdps[idx] = fopen(filenamep, "w");
        if (VL_UNLIKELY(!s_s.m_fdps[idx])) return 0;
        return (1 << idx);
    }
    static IData fdNew(const char* filenamep, const char* modep) VL_MT_SAFE {
        FILE* fp = fopen(filenamep, modep);
        if (VL_UNLIKELY(!fp)) return 0;
        // Bit 31 indicates it's a descriptor not a MCD
        VerilatedLockGuard lock(s_s.m_fdMutex);
        if (s_s.m_fdFree.empty()) {
            // Need to create more space in m_fdps and m_fdFree
            const size_t start = std::max(31ul + 1ul + 3ul, s_s.m_fdps.size());
            const size_t excess = 10;
            s_s.m_fdps.resize(start + excess);
            std::fill(s_s.m_fdps.begin() + start, s_s.m_fdps.end(), (FILE*)0);
            s_s.m_fdFree.resize(excess);
            for (int i = 0, id = start; i < s_s.m_fdFree.size(); i++, id++) {
                s_s.m_fdFree[i] = id;
            }
        }
        IData idx = s_s.m_fdFree.back();
        s_s.m_fdFree.pop_back();
        s_s.m_fdps[idx] = fp;
        return (idx | (1UL << 31));  // bit 31 indicates not MCD
    }
    static void fdFlush(IData fdi) VL_MT_SAFE {
        FILE* fp[30];
        const int n = fdToFp(fdi, fp, 30);
        VerilatedLockGuard lock(s_s.m_fdMutex);
        for (int i = 0; i < n; i++) fflush(fp[i]);
    }
    static IData fdSeek(IData fdi, IData offset, IData origin) VL_MT_SAFE {
        FILE* fp;
        const int n = fdToFp(fdi, &fp);
        VerilatedLockGuard lock(s_s.m_fdMutex);
        if (VL_UNLIKELY(!fp || (n != 1))) return 0;
        return static_cast<IData>(fseek(fp, static_cast<long>(offset), static_cast<int>(origin)));
    }
    static IData fdTell(IData fdi) VL_MT_SAFE {
        FILE* fp;
        const int n = fdToFp(fdi, &fp);
        VerilatedLockGuard lock(s_s.m_fdMutex);
        if (VL_UNLIKELY(!fp || (n != 1))) return 0;
        return static_cast<IData>(ftell(fp));
    }
    static void fdClose(IData fdi) VL_MT_SAFE {
        VerilatedLockGuard lock(s_s.m_fdMutex);
        if ((fdi & (1 << 31)) != 0) {
            // Non-MCD case
            IData idx = VL_MASK_I(31) & fdi;
            if (VL_UNLIKELY(idx >= s_s.m_fdps.size())) return;
            if (VL_UNLIKELY(!s_s.m_fdps[idx])) return;  // Already free
            fclose(s_s.m_fdps[idx]);
            s_s.m_fdps[idx] = (FILE*)0;
            s_s.m_fdFree.push_back(idx);
        } else {
            // MCD case
            for (int i = 0; (fdi != 0) && (i < 31); i++, fdi >>= 1) {
                if (fdi & VL_MASK_I(1)) {
                    fclose(s_s.m_fdps[i]);
                    s_s.m_fdps[i] = NULL;
                    s_s.m_fdFreeMct.push_back(i);
                }
            }
        }
    }
    static inline int fdToFp(IData fdi, FILE** fp, std::size_t max = 1) VL_MT_SAFE {
        if (VL_UNLIKELY(!fp || (max == 0))) return 0;
        VerilatedLockGuard lock(s_s.m_fdMutex);
        int out = 0;
        if ((fdi & (1 << 31)) != 0) {
            // Non-MCD case
            IData idx = fdi & VL_MASK_I(31);
            switch (idx) {
            case 0: fp[out++] = stdin; break;
            case 1: fp[out++] = stdout; break;
            case 2: fp[out++] = stderr; break;
            default:
                if (VL_LIKELY(idx < s_s.m_fdps.size())) fp[out++] = s_s.m_fdps[idx];
                break;
            }
        } else {
            // MCD Case
            for (int i = 0; (fdi != 0) && (out < max) && (i < 31); i++, fdi >>= 1)
                if (fdi & VL_MASK_I(1)) fp[out++] = s_s.m_fdps[i];
        }
        return out;
    }
};

//======================================================================

#endif  // Guard
