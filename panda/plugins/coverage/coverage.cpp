/* PANDABEGINCOMMENT
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */

#include <unordered_set>

#include "panda/plugin.h"

// OSI
#include "osi/osi_types.h"
#include "osi/osi_ext.h"

static FILE *coveragelog;

enum coverage_mode {
    MODE_PROCESS = 0,
    MODE_ASID = 1
};

struct RecordID {
    target_ulong pid_or_asid;
    target_ulong pc;
};

// Inject the hash function for RecordID into the std namespace, allows us to
// store RecordID in an unordered set.
namespace std
{
template <> struct hash<RecordID> {
    using argument_type = RecordID;
    using result_type = size_t;
    result_type operator()(argument_type const &s) const noexcept
    {
        // Combining hashes, see C++ reference:
        // https://en.cppreference.com/w/cpp/utility/hash
        result_type const h1 = std::hash<target_ulong>{}(s.pid_or_asid);
        result_type const h2 = std::hash<target_ulong>{}(s.pc);
        return h1 ^ (h2 << 1);
    }
};
} // namespace std

// Also needed to use an unordered set (probably in case there is a hash
// collision).
bool operator==(const RecordID &lhs, const RecordID &rhs)
{
    bool result = (lhs.pid_or_asid == rhs.pid_or_asid) && (lhs.pc == rhs.pc);
    return result;
}

coverage_mode mode = MODE_ASID;

int before_block_exec_process(CPUState *cpu, TranslationBlock *tb)
{
    // We keep track of pairs of PIDs and PCs that we've already seen
    // since we only need to write out distinct pairs once.
    static std::unordered_set<RecordID> seen;

    RecordID id;
    id.pc = tb->pc;

    // Get process id
    OsiThread *thread = get_current_thread(cpu);

    // Create the tuple of process id and program counter
    id.pid_or_asid = thread ? thread->pid : 0;
    target_ulong tid = thread->tid;
    free_osithread(thread);

    // Have we seen this block before?
    if (seen.find(id) == seen.end()) {

        // No!  Put it into the list.
        seen.insert(id);

        // Get the process name
        char *process_name = NULL;
        bool in_kernel = panda_in_kernel(first_cpu);
        if (!in_kernel) {
            OsiProc *proc = get_current_process(first_cpu);
            if (NULL != proc) {
                process_name = g_strdup(proc->name);
                free_osiproc(proc);
            } else {
                process_name = g_strdup("(unknown)");
            }
        } else {
            process_name = g_strdup("(kernel)");
        }

        // Log coverage data
        // process and thread ID are in decimal, as that is the radix
        // used by most tools that produce human readable output
        fprintf(coveragelog,
                "%s," TARGET_FMT_lu "," TARGET_FMT_lu ",%lu,0x"
                TARGET_FMT_lx ",%lu\n",
                process_name,
                id.pid_or_asid, tid, (uint64_t)in_kernel,
                tb->pc, (uint64_t)tb->size);
        g_free(process_name);
    }

    return 0;
}

int before_block_exec_asid(CPUState *cpu, TranslationBlock *tb)
{
    // We keep track of pairs of ASIDs and PCs that we've already seen
    // since we only need to write out distinct pairs once.
    static std::unordered_set<RecordID> seen;

    RecordID id;
    id.pc = tb->pc;

    id.pid_or_asid = panda_current_asid(first_cpu);
    if (seen.find(id) == seen.end()) {
        seen.insert(id);
        // want ASID to be output in hex to match what asidstory produces
        fprintf(coveragelog,
                "0x" TARGET_FMT_lx ",%lu,0x" TARGET_FMT_lx ",%lu\n",
                id.pid_or_asid,
                (uint64_t)panda_in_kernel(first_cpu), tb->pc,
                (uint64_t)tb->size);
    }

    return 0;
}

// These need to be extern "C" so that the ABI is compatible with QEMU/PANDA
extern "C" {
bool init_plugin(void *self);
void uninit_plugin(void *self);
}

bool init_plugin(void *self)
{
    // Get Plugin Arguments
    panda_arg_list *args = panda_get_args("coverage");
    const char *filename =
        panda_parse_string(args, "filename", "coverage.csv");

    const char *mode_arg;
    if (OS_UNKNOWN == panda_os_familyno) {
        mode_arg = panda_parse_string_opt(args, "mode", "asid",
                "type of segregation used for blocks (process or asid)");
    } else {
        mode_arg = panda_parse_string_opt(args, "mode", "process",
                "type of segregation used for blocks (process or asid)");
    }
    if (0 == strcmp(mode_arg, "asid")) {
        mode = MODE_ASID;
    } else if (0 == strcmp(mode_arg, "process")) {
        mode = MODE_PROCESS;
    } else {
        LOG_ERROR("invalid mode (%s) provided", mode_arg);
        return false;
    }

    uint32_t buffer_size = panda_parse_uint32_opt(args, "buffer_size", BUFSIZ,
        "size of output buffer (default=BUFSIZ)");
    // don't use LOG_INFO because I always want to see the informational
    // messages (which aren't on by default)
    printf("%susing buffer_size of %d\n", PANDA_MSG, buffer_size);

    if (MODE_PROCESS == mode) {
        if (OS_UNKNOWN == panda_os_familyno) {
            LOG_WARNING("no OS specified, switching to asid mode");
            mode = MODE_ASID;
        } else {
            printf("%susing mode process\n", PANDA_MSG);
            panda_require("osi");
            assert(init_osi_api());
        }
    } else {
        printf("%susing mode asid\n", PANDA_MSG);
    }

    // Open the coverage CSV file, and prepare the callback
    panda_cb pcb;
    coveragelog = fopen(filename, "w");
    if (BUFSIZ != buffer_size) {
        int buf_mode = _IOFBF;
        // if buffer_size is 0, then turn off buffering
        if (0 == buffer_size) {
            buf_mode = _IONBF;
        }
        // let setvbuf take care of allocating and freeing buffer
        int ret_code = setvbuf(coveragelog, NULL, buf_mode, buffer_size);
        if (0 != ret_code) {
            LOG_ERROR("could not change buffer size");
            return false;
        }
    }
    if (MODE_PROCESS == mode) {
        fprintf(coveragelog, "process\n");
        fprintf(coveragelog, "process name,process id,thread id,in kernel,"
                "block address,block size\n");
        pcb.before_block_exec = before_block_exec_process;
    } else {
        fprintf(coveragelog, "asid\n");
        fprintf(coveragelog, "asid,in kernel,block address,block size\n");
        pcb.before_block_exec = before_block_exec_asid;
    }

    // Register callback
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);

    return true;
}

void uninit_plugin(void *self) 
{
    fclose(coveragelog);
    coveragelog = NULL;
}
