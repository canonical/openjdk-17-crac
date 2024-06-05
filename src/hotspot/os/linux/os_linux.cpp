/*
 * Copyright (c) 1999, 2024, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2024 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// no precompiled headers
#include "jvm.h"
#include "classfile/vmSymbols.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "hugepages.hpp"
#include "interpreter/interpreter.hpp"
#include "jvmtifiles/jvmti.h"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/oopFactory.hpp"
#include "oops/oop.inline.hpp"
#include "oops/typeArrayOop.inline.hpp"
#include "os_linux.inline.hpp"
#include "os_posix.inline.hpp"
#include "os_share_linux.hpp"
#include "osContainer_linux.hpp"
#include "perfMemory_posix.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/osThread.hpp"
#include "runtime/perfMemory.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/statSampler.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threadCritical.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/timer.hpp"
#include "runtime/vm_version.hpp"
#include "signals_posix.hpp"
#include "semaphore_posix.hpp"
#include "services/heapDumper.hpp"
#include "services/memTracker.hpp"
#include "services/runtimeService.hpp"
#include "linuxAttachOperation.hpp"
#include "utilities/align.hpp"
#include "utilities/decoder.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/events.hpp"
#include "utilities/elfFile.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"
#include "utilities/powerOfTwo.hpp"
#include "utilities/vmError.hpp"
#include "attachListener_linux.hpp"

// put OS-includes here
# include <arpa/inet.h>
# include <sys/types.h>
# include <sys/mman.h>
# include <sys/stat.h>
# include <sys/select.h>
# include <sys/sysmacros.h>
# include <sys/wait.h>
# include <pthread.h>
# include <signal.h>
# include <endian.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <pthread.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/times.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <pwd.h>
# include <poll.h>
# include <fcntl.h>
# include <string.h>
# include <syscall.h>
# include <sys/sysinfo.h>
# include <sys/ipc.h>
# include <sys/shm.h>
# include <link.h>
# include <stdint.h>
# include <inttypes.h>
# include <sys/ioctl.h>
# include <linux/elf-em.h>
# include <sys/prctl.h>
# include <libgen.h>

#ifdef __GLIBC__
# include <malloc.h>
#endif

#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
  #include <sched.h>
  #undef _GNU_SOURCE
#else
  #include <sched.h>
#endif

// if RUSAGE_THREAD for getrusage() has not been defined, do it here. The code calling
// getrusage() is prepared to handle the associated failure.
#ifndef RUSAGE_THREAD
  #define RUSAGE_THREAD   (1)               /* only the calling thread */
#endif

#define MAX_PATH    (2 * K)

#define MAX_SECS 100000000

// for timer info max values which include all bits
#define ALL_64_BITS CONST64(0xFFFFFFFFFFFFFFFF)

#ifdef MUSL_LIBC
// dlvsym is not a part of POSIX
// and musl libc doesn't implement it.
static void *dlvsym(void *handle,
                    const char *symbol,
                    const char *version) {
   // load the latest version of symbol
   return dlsym(handle, symbol);
}
#endif

enum CoredumpFilterBit {
  FILE_BACKED_PVT_BIT = 1 << 2,
  FILE_BACKED_SHARED_BIT = 1 << 3,
  LARGEPAGES_BIT = 1 << 6,
  DAX_SHARED_BIT = 1 << 8
};

class FdsInfo {
public:

  enum state_t {
    INVALID = -3,
    CLOSED = -2,
    ROOT = -1,
    DUP_OF_0 = 0,
    // ...
  };

  enum mark_t {
    M_ZIP_CACHE    = 1 << 0,
    M_CANT_RESTORE = 1 << 1,
    M_CLASSPATH    = 1 << 2,
    M_PERSISTENT   = 1 << 3,
  };

private:
  struct fdinfo {
    struct stat stat;
    state_t state;
    unsigned mark;

    int flags;
  };

  bool same_fd(int fd1, int fd2);

  fdinfo *_fdinfos;
  int _len;

  void assert_mark(int i) {
    assert(inited(), "");
    assert(i < len(), "");
    assert(_fdinfos[i].state != CLOSED, "");
  }

public:
  void initialize();

  bool inited() { return _fdinfos != NULL; }
  int len() { return _len; }

  state_t get_state(int i, state_t orstate = INVALID) {
    assert(inited(), "");
    if (i < len()) {
      return _fdinfos[i].state;
    }
    guarantee(orstate != INVALID, "can't use default orstate");
    return orstate;
  }

  void set_state(int i, state_t newst) {
    assert(inited(), "");
    assert(i < len(), "");
    _fdinfos[i].state = newst;
  }

  void mark(int i, mark_t m) {
    assert_mark(i);
    _fdinfos[i].mark |= (unsigned)m;
  }
  void clear(int i, mark_t m) {
    assert_mark(i);
    _fdinfos[i].mark &= ~(unsigned)m;
  }
  bool check(int i, mark_t m) {
    assert_mark(i);
    return 0 != (_fdinfos[i].mark & (unsigned)m);
  }

  struct stat* get_stat(int i) {
    assert(inited(), "");
    assert(i < len(), "");
    return &_fdinfos[i].stat;
  }

  FdsInfo(bool do_init = true) :
    _fdinfos(NULL),
    _len(-1)
  {
    if (do_init) {
      initialize();
    }
  }

  ~FdsInfo() {
    if (_fdinfos) {
      FREE_C_HEAP_ARRAY(fdinfo, _fdinfos);
    }
  }
};

struct PersistentResourceDesc {
  int _fd;
  dev_t _st_dev;
  ino_t _st_ino;
  PersistentResourceDesc(int fd, int st_dev, int st_ino) :
    _fd(fd),
    _st_dev((dev_t)st_dev),
    _st_ino((ino_t)st_ino)
  {}

  PersistentResourceDesc() :
    _fd(INT_MAX)
  {}
};

struct CracFailDep {
  int _type;
  char* _msg;
  CracFailDep(int type, char* msg) :
    _type(type),
    _msg(msg)
  { }
  CracFailDep() :
    _type(JVM_CR_FAIL),
    _msg(NULL)
  { }
};

class CracRestoreParameters : public CHeapObj<mtInternal> {
 private:
  char* _raw_content;
  GrowableArray<const char *>* _properties;
  const char* _args;

  struct header {
    jlong _restore_time;
    jlong _restore_counter;
    int _nprops;
    int _env_memory_size;
  };


  static bool write_check_error(int fd, const void *buf, int count) {
    int wret = write(fd, buf, count);
    if (wret != count) {
      if (wret < 0) {
        perror("shm error");
      } else {
        fprintf(stderr, "write shm truncated");
      }
      return false;
    }
    return true;
  }

  static int system_props_length(const SystemProperty* props) {
    int len = 0;
    while (props != NULL) {
      ++len;
      props = props->next();
    }
    return len;
  }

  static int env_vars_size(const char* const * env) {
    int len = 0;
    for (; *env; ++env) {
      len += strlen(*env) + 1;
    }
    return len;
  }

 public:
  const char *args() const { return _args; }
  GrowableArray<const char *>* properties() const { return _properties; }

  CracRestoreParameters() :
    _raw_content(NULL),
    _properties(new (ResourceObj::C_HEAP, mtInternal) GrowableArray<const char *>(0, mtInternal)),
    _args(NULL)
  {}

  ~CracRestoreParameters() {
    if (_raw_content) {
      FREE_C_HEAP_ARRAY(char, _raw_content);
    }
    delete _properties;
  }

  static bool write_to(int fd,
      const SystemProperty* props,
      const char *args,
      jlong restore_time,
      jlong restore_counter) {
    header hdr = {
      restore_time,
      restore_counter,
      system_props_length(props),
      env_vars_size(environ)
    };

    if (!write_check_error(fd, (void *)&hdr, sizeof(header))) {
      return false;
    }

    const SystemProperty* p = props;
    while (p != NULL) {
      char prop[4096];
      int len = snprintf(prop, sizeof(prop), "%s=%s", p->key(), p->value());
      guarantee((0 < len) && ((unsigned)len < sizeof(prop)), "property does not fit temp buffer");
      if (!write_check_error(fd, prop, len+1)) {
        return false;
      }
      p = p->next();
    }

    // Write env vars
    for (char** env = environ; *env; ++env) {
      if (!write_check_error(fd, *env, strlen(*env) + 1)) {
        return false;
      }
    }

    return write_check_error(fd, args, strlen(args)+1); // +1 for null char
  }  

  bool read_from(int fd);
};


class VM_Crac: public VM_Operation {
  const bool _dry_run;
  bool _ok;
  GrowableArray<CracFailDep>* _failures;
  CracRestoreParameters _restore_parameters;
  outputStream* _ostream;
  LinuxAttachOperation* _attach_op;
 public:
  VM_Crac(bool dry_run, bufferedStream* jcmd_stream) :
    _dry_run(dry_run),
    _ok(false),
    _failures(new (ResourceObj::C_HEAP, mtInternal) GrowableArray<CracFailDep>(0, mtInternal)),
    _restore_parameters(),
    _ostream(jcmd_stream ? jcmd_stream : tty),
    _attach_op(jcmd_stream ? LinuxAttachListener::get_current_op() : NULL)
  { }

  ~VM_Crac() {
    delete _failures;
  }

  GrowableArray<CracFailDep>* failures() { return _failures; }

  bool ok() { return _ok; }
  const char* new_args() { return _restore_parameters.args(); }
  GrowableArray<const char *>* new_properties() { return _restore_parameters.properties(); }
  virtual bool allow_nested_vm_operations() const  { return true; }
  VMOp_Type type() const { return VMOp_VM_Crac; }
  void doit();
  bool read_shm(int shmid);

private:
  bool is_socket_from_jcmd(int sock_fd);
  void report_ok_to_jcmd_if_any();
  void print_resources(const char* msg, ...);
  void trace_cr(const char* msg, ...);
};

////////////////////////////////////////////////////////////////////////////////
// global variables
julong os::Linux::_physical_memory = 0;

address   os::Linux::_initial_thread_stack_bottom = NULL;
uintptr_t os::Linux::_initial_thread_stack_size   = 0;

int (*os::Linux::_pthread_getcpuclockid)(pthread_t, clockid_t *) = NULL;
int (*os::Linux::_pthread_setname_np)(pthread_t, const char*) = NULL;
pthread_t os::Linux::_main_thread;
int os::Linux::_page_size = -1;
bool os::Linux::_supports_fast_thread_cpu_time = false;
const char * os::Linux::_libc_version = NULL;
const char * os::Linux::_libpthread_version = NULL;

#ifdef __GLIBC__
os::Linux::mallinfo_func_t os::Linux::_mallinfo = NULL;
os::Linux::mallinfo2_func_t os::Linux::_mallinfo2 = NULL;
#endif // __GLIBC__

static jlong initial_time_count=0;

static int clock_tics_per_sec = 100;

// CRaC
static const char* _crengine = NULL;
static jlong _restore_start_time;
static jlong _restore_start_counter;
static FdsInfo _vm_inited_fds(false);
static GrowableArray<PersistentResourceDesc>* _persistent_resources = NULL;

// If the VM might have been created on the primordial thread, we need to resolve the
// primordial thread stack bounds and check if the current thread might be the
// primordial thread in places. If we know that the primordial thread is never used,
// such as when the VM was created by one of the standard java launchers, we can
// avoid this
static bool suppress_primordial_thread_resolution = false;

// utility functions

julong os::available_memory() {
  return Linux::available_memory();
}

julong os::Linux::available_memory() {
  // values in struct sysinfo are "unsigned long"
  struct sysinfo si;
  julong avail_mem;

  if (OSContainer::is_containerized()) {
    jlong mem_limit = OSContainer::memory_limit_in_bytes();
    jlong mem_usage;
    if (mem_limit > 0 && (mem_usage = OSContainer::memory_usage_in_bytes()) < 1) {
      log_debug(os, container)("container memory usage failed: " JLONG_FORMAT ", using host value", mem_usage);
    }
    if (mem_limit > 0 && mem_usage > 0) {
      avail_mem = mem_limit > mem_usage ? (julong)mem_limit - (julong)mem_usage : 0;
      log_trace(os)("available container memory: " JULONG_FORMAT, avail_mem);
      return avail_mem;
    }
  }

  sysinfo(&si);
  avail_mem = (julong)si.freeram * si.mem_unit;
  log_trace(os)("available memory: " JULONG_FORMAT, avail_mem);
  return avail_mem;
}

julong os::physical_memory() {
  jlong phys_mem = 0;
  if (OSContainer::is_containerized()) {
    jlong mem_limit;
    if ((mem_limit = OSContainer::memory_limit_in_bytes()) > 0) {
      log_trace(os)("total container memory: " JLONG_FORMAT, mem_limit);
      return mem_limit;
    }
  }

  phys_mem = Linux::physical_memory();
  log_trace(os)("total system memory: " JLONG_FORMAT, phys_mem);
  return phys_mem;
}

static uint64_t initial_total_ticks = 0;
static uint64_t initial_steal_ticks = 0;
static bool     has_initial_tick_info = false;

static void next_line(FILE *f) {
  int c;
  do {
    c = fgetc(f);
  } while (c != '\n' && c != EOF);
}

bool os::Linux::get_tick_information(CPUPerfTicks* pticks, int which_logical_cpu) {
  FILE*         fh;
  uint64_t      userTicks, niceTicks, systemTicks, idleTicks;
  // since at least kernel 2.6 : iowait: time waiting for I/O to complete
  // irq: time  servicing interrupts; softirq: time servicing softirqs
  uint64_t      iowTicks = 0, irqTicks = 0, sirqTicks= 0;
  // steal (since kernel 2.6.11): time spent in other OS when running in a virtualized environment
  uint64_t      stealTicks = 0;
  // guest (since kernel 2.6.24): time spent running a virtual CPU for guest OS under the
  // control of the Linux kernel
  uint64_t      guestNiceTicks = 0;
  int           logical_cpu = -1;
  const int     required_tickinfo_count = (which_logical_cpu == -1) ? 4 : 5;
  int           n;

  memset(pticks, 0, sizeof(CPUPerfTicks));

  if ((fh = fopen("/proc/stat", "r")) == NULL) {
    return false;
  }

  if (which_logical_cpu == -1) {
    n = fscanf(fh, "cpu " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " "
            UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " "
            UINT64_FORMAT " " UINT64_FORMAT " ",
            &userTicks, &niceTicks, &systemTicks, &idleTicks,
            &iowTicks, &irqTicks, &sirqTicks,
            &stealTicks, &guestNiceTicks);
  } else {
    // Move to next line
    next_line(fh);

    // find the line for requested cpu faster to just iterate linefeeds?
    for (int i = 0; i < which_logical_cpu; i++) {
      next_line(fh);
    }

    n = fscanf(fh, "cpu%u " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " "
               UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " " UINT64_FORMAT " "
               UINT64_FORMAT " " UINT64_FORMAT " ",
               &logical_cpu, &userTicks, &niceTicks,
               &systemTicks, &idleTicks, &iowTicks, &irqTicks, &sirqTicks,
               &stealTicks, &guestNiceTicks);
  }

  fclose(fh);
  if (n < required_tickinfo_count || logical_cpu != which_logical_cpu) {
    return false;
  }
  pticks->used       = userTicks + niceTicks;
  pticks->usedKernel = systemTicks + irqTicks + sirqTicks;
  pticks->total      = userTicks + niceTicks + systemTicks + idleTicks +
                       iowTicks + irqTicks + sirqTicks + stealTicks + guestNiceTicks;

  if (n > required_tickinfo_count + 3) {
    pticks->steal = stealTicks;
    pticks->has_steal_ticks = true;
  } else {
    pticks->steal = 0;
    pticks->has_steal_ticks = false;
  }

  return true;
}

// Return true if user is running as root.

bool os::have_special_privileges() {
  static bool init = false;
  static bool privileges = false;
  if (!init) {
    privileges = (getuid() != geteuid()) || (getgid() != getegid());
    init = true;
  }
  return privileges;
}


#ifndef SYS_gettid
// i386: 224, ia64: 1105, amd64: 186, sparc: 143
  #ifdef __ia64__
    #define SYS_gettid 1105
  #else
    #ifdef __i386__
      #define SYS_gettid 224
    #else
      #ifdef __amd64__
        #define SYS_gettid 186
      #else
        #ifdef __sparc__
          #define SYS_gettid 143
        #else
          #error define gettid for the arch
        #endif
      #endif
    #endif
  #endif
#endif


// pid_t gettid()
//
// Returns the kernel thread id of the currently running thread. Kernel
// thread id is used to access /proc.
pid_t os::Linux::gettid() {
  int rslt = syscall(SYS_gettid);
  assert(rslt != -1, "must be."); // old linuxthreads implementation?
  return (pid_t)rslt;
}

// Returns the amount of swap currently configured, in bytes.
// This can change at any time.
julong os::Linux::host_swap() {
  struct sysinfo si;
  sysinfo(&si);
  return (julong)(si.totalswap * si.mem_unit);
}

// Most versions of linux have a bug where the number of processors are
// determined by looking at the /proc file system.  In a chroot environment,
// the system call returns 1.
static bool unsafe_chroot_detected = false;
static const char *unstable_chroot_error = "/proc file system not found.\n"
                     "Java may be unstable running multithreaded in a chroot "
                     "environment on Linux when /proc filesystem is not mounted.";

void os::Linux::initialize_system_info() {
  set_processor_count(sysconf(_SC_NPROCESSORS_CONF));
  if (processor_count() == 1) {
    pid_t pid = os::Linux::gettid();
    char fname[32];
    jio_snprintf(fname, sizeof(fname), "/proc/%d", pid);
    FILE *fp = fopen(fname, "r");
    if (fp == NULL) {
      unsafe_chroot_detected = true;
    } else {
      fclose(fp);
    }
  }
  _physical_memory = (julong)sysconf(_SC_PHYS_PAGES) * (julong)sysconf(_SC_PAGESIZE);
  assert(processor_count() > 0, "linux error");
}

void os::init_system_properties_values() {
  // The next steps are taken in the product version:
  //
  // Obtain the JAVA_HOME value from the location of libjvm.so.
  // This library should be located at:
  // <JAVA_HOME>/lib/{client|server}/libjvm.so.
  //
  // If "/jre/lib/" appears at the right place in the path, then we
  // assume libjvm.so is installed in a JDK and we use this path.
  //
  // Otherwise exit with message: "Could not create the Java virtual machine."
  //
  // The following extra steps are taken in the debugging version:
  //
  // If "/jre/lib/" does NOT appear at the right place in the path
  // instead of exit check for $JAVA_HOME environment variable.
  //
  // If it is defined and we are able to locate $JAVA_HOME/jre/lib/<arch>,
  // then we append a fake suffix "hotspot/libjvm.so" to this path so
  // it looks like libjvm.so is installed there
  // <JAVA_HOME>/jre/lib/<arch>/hotspot/libjvm.so.
  //
  // Otherwise exit.
  //
  // Important note: if the location of libjvm.so changes this
  // code needs to be changed accordingly.

  // See ld(1):
  //      The linker uses the following search paths to locate required
  //      shared libraries:
  //        1: ...
  //        ...
  //        7: The default directories, normally /lib and /usr/lib.
#ifndef OVERRIDE_LIBPATH
  #if defined(_LP64)
    #define DEFAULT_LIBPATH "/usr/lib64:/lib64:/lib:/usr/lib"
  #else
    #define DEFAULT_LIBPATH "/lib:/usr/lib"
  #endif
#else
  #define DEFAULT_LIBPATH OVERRIDE_LIBPATH
#endif

// Base path of extensions installed on the system.
#define SYS_EXT_DIR     "/usr/java/packages"
#define EXTENSIONS_DIR  "/lib/ext"

  // Buffer that fits several sprintfs.
  // Note that the space for the colon and the trailing null are provided
  // by the nulls included by the sizeof operator.
  const size_t bufsize =
    MAX2((size_t)MAXPATHLEN,  // For dll_dir & friends.
         (size_t)MAXPATHLEN + sizeof(EXTENSIONS_DIR) + sizeof(SYS_EXT_DIR) + sizeof(EXTENSIONS_DIR)); // extensions dir
  char *buf = NEW_C_HEAP_ARRAY(char, bufsize, mtInternal);

  // sysclasspath, java_home, dll_dir
  {
    char *pslash;
    os::jvm_path(buf, bufsize);

    // Found the full path to libjvm.so.
    // Now cut the path to <java_home>/jre if we can.
    pslash = strrchr(buf, '/');
    if (pslash != NULL) {
      *pslash = '\0';            // Get rid of /libjvm.so.
    }
    pslash = strrchr(buf, '/');
    if (pslash != NULL) {
      *pslash = '\0';            // Get rid of /{client|server|hotspot}.
    }
    Arguments::set_dll_dir(buf);

    if (pslash != NULL) {
      pslash = strrchr(buf, '/');
      if (pslash != NULL) {
        *pslash = '\0';        // Get rid of /lib.
      }
    }
    Arguments::set_java_home(buf);
    if (!set_boot_path('/', ':')) {
      vm_exit_during_initialization("Failed setting boot class path.", NULL);
    }
  }

  // Where to look for native libraries.
  //
  // Note: Due to a legacy implementation, most of the library path
  // is set in the launcher. This was to accomodate linking restrictions
  // on legacy Linux implementations (which are no longer supported).
  // Eventually, all the library path setting will be done here.
  //
  // However, to prevent the proliferation of improperly built native
  // libraries, the new path component /usr/java/packages is added here.
  // Eventually, all the library path setting will be done here.
  {
    // Get the user setting of LD_LIBRARY_PATH, and prepended it. It
    // should always exist (until the legacy problem cited above is
    // addressed).
    const char *v = ::getenv("LD_LIBRARY_PATH");
    const char *v_colon = ":";
    if (v == NULL) { v = ""; v_colon = ""; }
    // That's +1 for the colon and +1 for the trailing '\0'.
    char *ld_library_path = NEW_C_HEAP_ARRAY(char,
                                             strlen(v) + 1 +
                                             sizeof(SYS_EXT_DIR) + sizeof("/lib/") + sizeof(DEFAULT_LIBPATH) + 1,
                                             mtInternal);
    sprintf(ld_library_path, "%s%s" SYS_EXT_DIR "/lib:" DEFAULT_LIBPATH, v, v_colon);
    Arguments::set_library_path(ld_library_path);
    FREE_C_HEAP_ARRAY(char, ld_library_path);
  }

  // Extensions directories.
  sprintf(buf, "%s" EXTENSIONS_DIR ":" SYS_EXT_DIR EXTENSIONS_DIR, Arguments::get_java_home());
  Arguments::set_ext_dirs(buf);

  FREE_C_HEAP_ARRAY(char, buf);

#undef DEFAULT_LIBPATH
#undef SYS_EXT_DIR
#undef EXTENSIONS_DIR
}

////////////////////////////////////////////////////////////////////////////////
// breakpoint support

void os::breakpoint() {
  BREAKPOINT;
}

extern "C" void breakpoint() {
  // use debugger to set breakpoint here
}

//////////////////////////////////////////////////////////////////////////////
// detecting pthread library

void os::Linux::libpthread_init() {
  // Save glibc and pthread version strings.
#if !defined(_CS_GNU_LIBC_VERSION) || \
    !defined(_CS_GNU_LIBPTHREAD_VERSION)
  #error "glibc too old (< 2.3.2)"
#endif

#ifdef MUSL_LIBC
  // confstr() from musl libc returns EINVAL for
  // _CS_GNU_LIBC_VERSION and _CS_GNU_LIBPTHREAD_VERSION
  os::Linux::set_libc_version("musl - unknown");
  os::Linux::set_libpthread_version("musl - unknown");
#else
  size_t n = confstr(_CS_GNU_LIBC_VERSION, NULL, 0);
  assert(n > 0, "cannot retrieve glibc version");
  char *str = (char *)malloc(n, mtInternal);
  confstr(_CS_GNU_LIBC_VERSION, str, n);
  os::Linux::set_libc_version(str);

  n = confstr(_CS_GNU_LIBPTHREAD_VERSION, NULL, 0);
  assert(n > 0, "cannot retrieve pthread version");
  str = (char *)malloc(n, mtInternal);
  confstr(_CS_GNU_LIBPTHREAD_VERSION, str, n);
  os::Linux::set_libpthread_version(str);
#endif
}

/////////////////////////////////////////////////////////////////////////////
// thread stack expansion

// os::Linux::manually_expand_stack() takes care of expanding the thread
// stack. Note that this is normally not needed: pthread stacks allocate
// thread stack using mmap() without MAP_NORESERVE, so the stack is already
// committed. Therefore it is not necessary to expand the stack manually.
//
// Manually expanding the stack was historically needed on LinuxThreads
// thread stacks, which were allocated with mmap(MAP_GROWSDOWN). Nowadays
// it is kept to deal with very rare corner cases:
//
// For one, user may run the VM on an own implementation of threads
// whose stacks are - like the old LinuxThreads - implemented using
// mmap(MAP_GROWSDOWN).
//
// Also, this coding may be needed if the VM is running on the primordial
// thread. Normally we avoid running on the primordial thread; however,
// user may still invoke the VM on the primordial thread.
//
// The following historical comment describes the details about running
// on a thread stack allocated with mmap(MAP_GROWSDOWN):


// Force Linux kernel to expand current thread stack. If "bottom" is close
// to the stack guard, caller should block all signals.
//
// MAP_GROWSDOWN:
//   A special mmap() flag that is used to implement thread stacks. It tells
//   kernel that the memory region should extend downwards when needed. This
//   allows early versions of LinuxThreads to only mmap the first few pages
//   when creating a new thread. Linux kernel will automatically expand thread
//   stack as needed (on page faults).
//
//   However, because the memory region of a MAP_GROWSDOWN stack can grow on
//   demand, if a page fault happens outside an already mapped MAP_GROWSDOWN
//   region, it's hard to tell if the fault is due to a legitimate stack
//   access or because of reading/writing non-exist memory (e.g. buffer
//   overrun). As a rule, if the fault happens below current stack pointer,
//   Linux kernel does not expand stack, instead a SIGSEGV is sent to the
//   application (see Linux kernel fault.c).
//
//   This Linux feature can cause SIGSEGV when VM bangs thread stack for
//   stack overflow detection.
//
//   Newer version of LinuxThreads (since glibc-2.2, or, RH-7.x) and NPTL do
//   not use MAP_GROWSDOWN.
//
// To get around the problem and allow stack banging on Linux, we need to
// manually expand thread stack after receiving the SIGSEGV.
//
// There are two ways to expand thread stack to address "bottom", we used
// both of them in JVM before 1.5:
//   1. adjust stack pointer first so that it is below "bottom", and then
//      touch "bottom"
//   2. mmap() the page in question
//
// Now alternate signal stack is gone, it's harder to use 2. For instance,
// if current sp is already near the lower end of page 101, and we need to
// call mmap() to map page 100, it is possible that part of the mmap() frame
// will be placed in page 100. When page 100 is mapped, it is zero-filled.
// That will destroy the mmap() frame and cause VM to crash.
//
// The following code works by adjusting sp first, then accessing the "bottom"
// page to force a page fault. Linux kernel will then automatically expand the
// stack mapping.
//
// _expand_stack_to() assumes its frame size is less than page size, which
// should always be true if the function is not inlined.

static void NOINLINE _expand_stack_to(address bottom) {
  address sp;
  size_t size;
  volatile char *p;

  // Adjust bottom to point to the largest address within the same page, it
  // gives us a one-page buffer if alloca() allocates slightly more memory.
  bottom = (address)align_down((uintptr_t)bottom, os::Linux::page_size());
  bottom += os::Linux::page_size() - 1;

  // sp might be slightly above current stack pointer; if that's the case, we
  // will alloca() a little more space than necessary, which is OK. Don't use
  // os::current_stack_pointer(), as its result can be slightly below current
  // stack pointer, causing us to not alloca enough to reach "bottom".
  sp = (address)&sp;

  if (sp > bottom) {
    size = sp - bottom;
    p = (volatile char *)alloca(size);
    assert(p != NULL && p <= (volatile char *)bottom, "alloca problem?");
    p[0] = '\0';
  }
}

void os::Linux::expand_stack_to(address bottom) {
  _expand_stack_to(bottom);
}

bool os::Linux::manually_expand_stack(JavaThread * t, address addr) {
  assert(t!=NULL, "just checking");
  assert(t->osthread()->expanding_stack(), "expand should be set");

  if (t->is_in_usable_stack(addr)) {
    sigset_t mask_all, old_sigset;
    sigfillset(&mask_all);
    pthread_sigmask(SIG_SETMASK, &mask_all, &old_sigset);
    _expand_stack_to(addr);
    pthread_sigmask(SIG_SETMASK, &old_sigset, NULL);
    return true;
  }
  return false;
}

//////////////////////////////////////////////////////////////////////////////
// create new thread

// Thread start routine for all newly created threads
static void *thread_native_entry(Thread *thread) {

  thread->record_stack_base_and_size();

#ifndef __GLIBC__
  // Try to randomize the cache line index of hot stack frames.
  // This helps when threads of the same stack traces evict each other's
  // cache lines. The threads can be either from the same JVM instance, or
  // from different JVM instances. The benefit is especially true for
  // processors with hyperthreading technology.
  // This code is not needed anymore in glibc because it has MULTI_PAGE_ALIASING
  // and we did not see any degradation in performance without `alloca()`.
  static int counter = 0;
  int pid = os::current_process_id();
  int random = ((pid ^ counter++) & 7) * 128;
  void *stackmem = alloca(random != 0 ? random : 1); // ensure we allocate > 0
  // Ensure the alloca result is used in a way that prevents the compiler from eliding it.
  *(char *)stackmem = 1;
#endif

  thread->initialize_thread_current();

  OSThread* osthread = thread->osthread();
  Monitor* sync = osthread->startThread_lock();

  osthread->set_thread_id(os::current_thread_id());

  if (UseNUMA) {
    int lgrp_id = os::numa_get_group_id();
    if (lgrp_id != -1) {
      thread->set_lgrp_id(lgrp_id);
    }
  }
  // initialize signal mask for this thread
  PosixSignals::hotspot_sigmask(thread);

  // initialize floating point control register
  os::Linux::init_thread_fpu_state();

  // handshaking with parent thread
  {
    MutexLocker ml(sync, Mutex::_no_safepoint_check_flag);

    // notify parent thread
    osthread->set_state(INITIALIZED);
    sync->notify_all();

    // wait until os::start_thread()
    while (osthread->get_state() == INITIALIZED) {
      sync->wait_without_safepoint_check();
    }
  }

  log_info(os, thread)("Thread is alive (tid: " UINTX_FORMAT ", pthread id: " UINTX_FORMAT ").",
    os::current_thread_id(), (uintx) pthread_self());

  assert(osthread->pthread_id() != 0, "pthread_id was not set as expected");

  if (DelayThreadStartALot) {
    os::naked_short_sleep(100);
  }

  // call one more level start routine
  thread->call_run();

  // Note: at this point the thread object may already have deleted itself.
  // Prevent dereferencing it from here on out.
  thread = NULL;

  log_info(os, thread)("Thread finished (tid: " UINTX_FORMAT ", pthread id: " UINTX_FORMAT ").",
    os::current_thread_id(), (uintx) pthread_self());

  return 0;
}

// On Linux, glibc places static TLS blocks (for __thread variables) on
// the thread stack. This decreases the stack size actually available
// to threads.
//
// For large static TLS sizes, this may cause threads to malfunction due
// to insufficient stack space. This is a well-known issue in glibc:
// http://sourceware.org/bugzilla/show_bug.cgi?id=11787.
//
// As a workaround, we call a private but assumed-stable glibc function,
// __pthread_get_minstack() to obtain the minstack size and derive the
// static TLS size from it. We then increase the user requested stack
// size by this TLS size. The same function is used to determine whether
// adjustStackSizeForGuardPages() needs to be true.
//
// Due to compatibility concerns, this size adjustment is opt-in and
// controlled via AdjustStackSizeForTLS.
typedef size_t (*GetMinStack)(const pthread_attr_t *attr);

GetMinStack _get_minstack_func = nullptr;  // Initialized via os::init_2()

// Returns the size of the static TLS area glibc puts on thread stacks.
// The value is cached on first use, which occurs when the first thread
// is created during VM initialization.
static size_t get_static_tls_area_size(const pthread_attr_t *attr) {
  size_t tls_size = 0;
  if (_get_minstack_func != NULL) {
    // Obtain the pthread minstack size by calling __pthread_get_minstack.
    size_t minstack_size = _get_minstack_func(attr);

    // Remove non-TLS area size included in minstack size returned
    // by __pthread_get_minstack() to get the static TLS size.
    // If adjustStackSizeForGuardPages() is true, minstack size includes
    // guard_size. Otherwise guard_size is automatically added
    // to the stack size by pthread_create and is no longer included
    // in minstack size. In both cases, the guard_size is taken into
    // account, so there is no need to adjust the result for that.
    //
    // Although __pthread_get_minstack() is a private glibc function,
    // it is expected to have a stable behavior across future glibc
    // versions while glibc still allocates the static TLS blocks off
    // the stack. Following is glibc 2.28 __pthread_get_minstack():
    //
    // size_t
    // __pthread_get_minstack (const pthread_attr_t *attr)
    // {
    //   return GLRO(dl_pagesize) + __static_tls_size + PTHREAD_STACK_MIN;
    // }
    //
    //
    // The following 'minstack_size > os::vm_page_size() + PTHREAD_STACK_MIN'
    // if check is done for precaution.
    if (minstack_size > (size_t)os::vm_page_size() + PTHREAD_STACK_MIN) {
      tls_size = minstack_size - os::vm_page_size() - PTHREAD_STACK_MIN;
    }
  }

  log_info(os, thread)("Stack size adjustment for TLS is " SIZE_FORMAT,
                       tls_size);
  return tls_size;
}

// In glibc versions prior to 2.27 the guard size mechanism
// was not implemented properly. The POSIX standard requires adding
// the size of the guard pages to the stack size, instead glibc
// took the space out of 'stacksize'. Thus we need to adapt the requested
// stack_size by the size of the guard pages to mimic proper behaviour.
// The fix in glibc 2.27 has now been backported to numerous earlier
// glibc versions so we need to do a dynamic runtime check.
static bool _adjustStackSizeForGuardPages = true;
bool os::Linux::adjustStackSizeForGuardPages() {
  return _adjustStackSizeForGuardPages;
}

#ifdef __GLIBC__
static void init_adjust_stacksize_for_guard_pages() {
  assert(_get_minstack_func == nullptr, "initialization error");
  _get_minstack_func =(GetMinStack)dlsym(RTLD_DEFAULT, "__pthread_get_minstack");
  log_info(os, thread)("Lookup of __pthread_get_minstack %s",
                       _get_minstack_func == nullptr ? "failed" : "succeeded");

  if (_get_minstack_func != nullptr) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t min_stack = _get_minstack_func(&attr);
    size_t guard = 16 * K; // Actual value doesn't matter as it is not examined
    pthread_attr_setguardsize(&attr, guard);
    size_t min_stack2 = _get_minstack_func(&attr);
    pthread_attr_destroy(&attr);
    // If the minimum stack size changed when we added the guard page space
    // then we need to perform the adjustment.
    _adjustStackSizeForGuardPages = (min_stack2 != min_stack);
    log_info(os)("Glibc stack size guard page adjustment is %sneeded",
                 _adjustStackSizeForGuardPages ? "" : "not ");
  }
}
#endif // GLIBC

bool os::create_thread(Thread* thread, ThreadType thr_type,
                       size_t req_stack_size) {
  assert(thread->osthread() == NULL, "caller responsible");

  // Allocate the OSThread object
  OSThread* osthread = new OSThread(NULL, NULL);
  if (osthread == NULL) {
    return false;
  }

  // set the correct thread state
  osthread->set_thread_type(thr_type);

  // Initial state is ALLOCATED but not INITIALIZED
  osthread->set_state(ALLOCATED);

  thread->set_osthread(osthread);

  // init thread attributes
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  // Calculate stack size if it's not specified by caller.
  size_t stack_size = os::Posix::get_initial_stack_size(thr_type, req_stack_size);
  size_t guard_size = os::Linux::default_guard_size(thr_type);

  // Configure glibc guard page. Must happen before calling
  // get_static_tls_area_size(), which uses the guard_size.
  pthread_attr_setguardsize(&attr, guard_size);

  // Apply stack size adjustments if needed. However, be careful not to end up
  // with a size of zero due to overflow. Don't add the adjustment in that case.
  size_t stack_adjust_size = 0;
  if (AdjustStackSizeForTLS) {
    // Adjust the stack_size for on-stack TLS - see get_static_tls_area_size().
    stack_adjust_size += get_static_tls_area_size(&attr);
  } else if (os::Linux::adjustStackSizeForGuardPages()) {
    stack_adjust_size += guard_size;
  }

  stack_adjust_size = align_up(stack_adjust_size, os::vm_page_size());
  if (stack_size <= SIZE_MAX - stack_adjust_size) {
    stack_size += stack_adjust_size;
  }
  assert(is_aligned(stack_size, os::vm_page_size()), "stack_size not aligned");

  if (THPStackMitigation) {
    // In addition to the glibc guard page that prevents inter-thread-stack hugepage
    // coalescing (see comment in os::Linux::default_guard_size()), we also make
    // sure the stack size itself is not huge-page-size aligned; that makes it much
    // more likely for thread stack boundaries to be unaligned as well and hence
    // protects thread stacks from being targeted by khugepaged.
    if (HugePages::thp_pagesize() > 0 &&
        is_aligned(stack_size, HugePages::thp_pagesize())) {
      stack_size += os::vm_page_size();
    }
  }

  int status = pthread_attr_setstacksize(&attr, stack_size);
  if (status != 0) {
    // pthread_attr_setstacksize() function can fail
    // if the stack size exceeds a system-imposed limit.
    assert_status(status == EINVAL, status, "pthread_attr_setstacksize");
    log_warning(os, thread)("The %sthread stack size specified is invalid: " SIZE_FORMAT "k",
                            (thr_type == compiler_thread) ? "compiler " : ((thr_type == java_thread) ? "" : "VM "),
                            stack_size / K);
    thread->set_osthread(NULL);
    delete osthread;
    return false;
  }

  ThreadState state;

  {
    ResourceMark rm;
    pthread_t tid;
    int ret = 0;
    int limit = 3;
    do {
      ret = pthread_create(&tid, &attr, (void* (*)(void*)) thread_native_entry, thread);
    } while (ret == EAGAIN && limit-- > 0);

    char buf[64];
    if (ret == 0) {
      log_info(os, thread)("Thread \"%s\" started (pthread id: " UINTX_FORMAT ", attributes: %s). ",
                           thread->name(), (uintx) tid, os::Posix::describe_pthread_attr(buf, sizeof(buf), &attr));

      // Print current timer slack if override is enabled and timer slack value is available.
      // Avoid calling prctl otherwise for extra safety.
      if (TimerSlack >= 0) {
        int slack = prctl(PR_GET_TIMERSLACK);
        if (slack >= 0) {
          log_info(os, thread)("Thread \"%s\" (pthread id: " UINTX_FORMAT ") timer slack: %dns",
                               thread->name(), (uintx) tid, slack);
        }
      }
    } else {
      log_warning(os, thread)("Failed to start thread \"%s\" - pthread_create failed (%s) for attributes: %s.",
                              thread->name(), os::errno_name(ret), os::Posix::describe_pthread_attr(buf, sizeof(buf), &attr));
      // Log some OS information which might explain why creating the thread failed.
      log_info(os, thread)("Number of threads approx. running in the VM: %d", Threads::number_of_threads());
      LogStream st(Log(os, thread)::info());
      os::Posix::print_rlimit_info(&st);
      os::print_memory_info(&st);
      os::Linux::print_proc_sys_info(&st);
      os::Linux::print_container_info(&st);
    }

    pthread_attr_destroy(&attr);

    if (ret != 0) {
      // Need to clean up stuff we've allocated so far
      thread->set_osthread(NULL);
      delete osthread;
      return false;
    }

    // Store pthread info into the OSThread
    osthread->set_pthread_id(tid);

    // Wait until child thread is either initialized or aborted
    {
      Monitor* sync_with_child = osthread->startThread_lock();
      MutexLocker ml(sync_with_child, Mutex::_no_safepoint_check_flag);
      while ((state = osthread->get_state()) == ALLOCATED) {
        sync_with_child->wait_without_safepoint_check();
      }
    }
  }

  // The thread is returned suspended (in state INITIALIZED),
  // and is started higher up in the call chain
  assert(state == INITIALIZED, "race condition");
  return true;
}

/////////////////////////////////////////////////////////////////////////////
// attach existing thread

// bootstrap the main thread
bool os::create_main_thread(JavaThread* thread) {
  assert(os::Linux::_main_thread == pthread_self(), "should be called inside main thread");
  return create_attached_thread(thread);
}

bool os::create_attached_thread(JavaThread* thread) {
#ifdef ASSERT
  thread->verify_not_published();
#endif

  // Allocate the OSThread object
  OSThread* osthread = new OSThread(NULL, NULL);

  if (osthread == NULL) {
    return false;
  }

  // Store pthread info into the OSThread
  osthread->set_thread_id(os::Linux::gettid());
  osthread->set_pthread_id(::pthread_self());

  // initialize floating point control register
  os::Linux::init_thread_fpu_state();

  // Initial thread state is RUNNABLE
  osthread->set_state(RUNNABLE);

  thread->set_osthread(osthread);

  if (UseNUMA) {
    int lgrp_id = os::numa_get_group_id();
    if (lgrp_id != -1) {
      thread->set_lgrp_id(lgrp_id);
    }
  }

  if (os::is_primordial_thread()) {
    // If current thread is primordial thread, its stack is mapped on demand,
    // see notes about MAP_GROWSDOWN. Here we try to force kernel to map
    // the entire stack region to avoid SEGV in stack banging.
    // It is also useful to get around the heap-stack-gap problem on SuSE
    // kernel (see 4821821 for details). We first expand stack to the top
    // of yellow zone, then enable stack yellow zone (order is significant,
    // enabling yellow zone first will crash JVM on SuSE Linux), so there
    // is no gap between the last two virtual memory regions.

    StackOverflow* overflow_state = thread->stack_overflow_state();
    address addr = overflow_state->stack_reserved_zone_base();
    assert(addr != NULL, "initialization problem?");
    assert(overflow_state->stack_available(addr) > 0, "stack guard should not be enabled");

    osthread->set_expanding_stack();
    os::Linux::manually_expand_stack(thread, addr);
    osthread->clear_expanding_stack();
  }

  // initialize signal mask for this thread
  // and save the caller's signal mask
  PosixSignals::hotspot_sigmask(thread);

  log_info(os, thread)("Thread attached (tid: " UINTX_FORMAT ", pthread id: " UINTX_FORMAT
                       ", stack: " PTR_FORMAT " - " PTR_FORMAT " (" SIZE_FORMAT "K) ).",
                       os::current_thread_id(), (uintx) pthread_self(),
                       p2i(thread->stack_base()), p2i(thread->stack_end()), thread->stack_size() / K);

  return true;
}

void os::pd_start_thread(Thread* thread) {
  OSThread * osthread = thread->osthread();
  assert(osthread->get_state() != INITIALIZED, "just checking");
  Monitor* sync_with_child = osthread->startThread_lock();
  MutexLocker ml(sync_with_child, Mutex::_no_safepoint_check_flag);
  sync_with_child->notify();
}

// Free Linux resources related to the OSThread
void os::free_thread(OSThread* osthread) {
  assert(osthread != NULL, "osthread not set");

  // We are told to free resources of the argument thread,
  // but we can only really operate on the current thread.
  assert(Thread::current()->osthread() == osthread,
         "os::free_thread but not current thread");

#ifdef ASSERT
  sigset_t current;
  sigemptyset(&current);
  pthread_sigmask(SIG_SETMASK, NULL, &current);
  assert(!sigismember(&current, PosixSignals::SR_signum), "SR signal should not be blocked!");
#endif

  // Restore caller's signal mask
  sigset_t sigmask = osthread->caller_sigmask();
  pthread_sigmask(SIG_SETMASK, &sigmask, NULL);

  delete osthread;
}

//////////////////////////////////////////////////////////////////////////////
// primordial thread

// Check if current thread is the primordial thread, similar to Solaris thr_main.
bool os::is_primordial_thread(void) {
  if (suppress_primordial_thread_resolution) {
    return false;
  }
  char dummy;
  // If called before init complete, thread stack bottom will be null.
  // Can be called if fatal error occurs before initialization.
  if (os::Linux::initial_thread_stack_bottom() == NULL) return false;
  assert(os::Linux::initial_thread_stack_bottom() != NULL &&
         os::Linux::initial_thread_stack_size()   != 0,
         "os::init did not locate primordial thread's stack region");
  if ((address)&dummy >= os::Linux::initial_thread_stack_bottom() &&
      (address)&dummy < os::Linux::initial_thread_stack_bottom() +
                        os::Linux::initial_thread_stack_size()) {
    return true;
  } else {
    return false;
  }
}

// Find the virtual memory area that contains addr
static bool find_vma(address addr, address* vma_low, address* vma_high) {
  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp) {
    address low, high;
    while (!feof(fp)) {
      if (fscanf(fp, "%p-%p", &low, &high) == 2) {
        if (low <= addr && addr < high) {
          if (vma_low)  *vma_low  = low;
          if (vma_high) *vma_high = high;
          fclose(fp);
          return true;
        }
      }
      for (;;) {
        int ch = fgetc(fp);
        if (ch == EOF || ch == (int)'\n') break;
      }
    }
    fclose(fp);
  }
  return false;
}

// Locate primordial thread stack. This special handling of primordial thread stack
// is needed because pthread_getattr_np() on most (all?) Linux distros returns
// bogus value for the primordial process thread. While the launcher has created
// the VM in a new thread since JDK 6, we still have to allow for the use of the
// JNI invocation API from a primordial thread.
void os::Linux::capture_initial_stack(size_t max_size) {

  // max_size is either 0 (which means accept OS default for thread stacks) or
  // a user-specified value known to be at least the minimum needed. If we
  // are actually on the primordial thread we can make it appear that we have a
  // smaller max_size stack by inserting the guard pages at that location. But we
  // cannot do anything to emulate a larger stack than what has been provided by
  // the OS or threading library. In fact if we try to use a stack greater than
  // what is set by rlimit then we will crash the hosting process.

  // Maximum stack size is the easy part, get it from RLIMIT_STACK.
  // If this is "unlimited" then it will be a huge value.
  struct rlimit rlim;
  getrlimit(RLIMIT_STACK, &rlim);
  size_t stack_size = rlim.rlim_cur;

  // 6308388: a bug in ld.so will relocate its own .data section to the
  //   lower end of primordial stack; reduce ulimit -s value a little bit
  //   so we won't install guard page on ld.so's data section.
  //   But ensure we don't underflow the stack size - allow 1 page spare
  if (stack_size >= (size_t)(3 * page_size())) {
    stack_size -= 2 * page_size();
  }

  // Try to figure out where the stack base (top) is. This is harder.
  //
  // When an application is started, glibc saves the initial stack pointer in
  // a global variable "__libc_stack_end", which is then used by system
  // libraries. __libc_stack_end should be pretty close to stack top. The
  // variable is available since the very early days. However, because it is
  // a private interface, it could disappear in the future.
  //
  // Linux kernel saves start_stack information in /proc/<pid>/stat. Similar
  // to __libc_stack_end, it is very close to stack top, but isn't the real
  // stack top. Note that /proc may not exist if VM is running as a chroot
  // program, so reading /proc/<pid>/stat could fail. Also the contents of
  // /proc/<pid>/stat could change in the future (though unlikely).
  //
  // We try __libc_stack_end first. If that doesn't work, look for
  // /proc/<pid>/stat. If neither of them works, we use current stack pointer
  // as a hint, which should work well in most cases.

  uintptr_t stack_start;

  // try __libc_stack_end first
  uintptr_t *p = (uintptr_t *)dlsym(RTLD_DEFAULT, "__libc_stack_end");
  if (p && *p) {
    stack_start = *p;
  } else {
    // see if we can get the start_stack field from /proc/self/stat
    FILE *fp;
    int pid;
    char state;
    int ppid;
    int pgrp;
    int session;
    int nr;
    int tpgrp;
    unsigned long flags;
    unsigned long minflt;
    unsigned long cminflt;
    unsigned long majflt;
    unsigned long cmajflt;
    unsigned long utime;
    unsigned long stime;
    long cutime;
    long cstime;
    long prio;
    long nice;
    long junk;
    long it_real;
    uintptr_t start;
    uintptr_t vsize;
    intptr_t rss;
    uintptr_t rsslim;
    uintptr_t scodes;
    uintptr_t ecode;
    int i;

    // Figure what the primordial thread stack base is. Code is inspired
    // by email from Hans Boehm. /proc/self/stat begins with current pid,
    // followed by command name surrounded by parentheses, state, etc.
    char stat[2048];
    int statlen;

    fp = fopen("/proc/self/stat", "r");
    if (fp) {
      statlen = fread(stat, 1, 2047, fp);
      stat[statlen] = '\0';
      fclose(fp);

      // Skip pid and the command string. Note that we could be dealing with
      // weird command names, e.g. user could decide to rename java launcher
      // to "java 1.4.2 :)", then the stat file would look like
      //                1234 (java 1.4.2 :)) R ... ...
      // We don't really need to know the command string, just find the last
      // occurrence of ")" and then start parsing from there. See bug 4726580.
      char * s = strrchr(stat, ')');

      i = 0;
      if (s) {
        // Skip blank chars
        do { s++; } while (s && isspace(*s));

#define _UFM UINTX_FORMAT
#define _DFM INTX_FORMAT

        //                                     1   1   1   1   1   1   1   1   1   1   2   2    2    2    2    2    2    2    2
        //              3  4  5  6  7  8   9   0   1   2   3   4   5   6   7   8   9   0   1    2    3    4    5    6    7    8
        i = sscanf(s, "%c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld " _UFM _UFM _DFM _UFM _UFM _UFM _UFM,
                   &state,          // 3  %c
                   &ppid,           // 4  %d
                   &pgrp,           // 5  %d
                   &session,        // 6  %d
                   &nr,             // 7  %d
                   &tpgrp,          // 8  %d
                   &flags,          // 9  %lu
                   &minflt,         // 10 %lu
                   &cminflt,        // 11 %lu
                   &majflt,         // 12 %lu
                   &cmajflt,        // 13 %lu
                   &utime,          // 14 %lu
                   &stime,          // 15 %lu
                   &cutime,         // 16 %ld
                   &cstime,         // 17 %ld
                   &prio,           // 18 %ld
                   &nice,           // 19 %ld
                   &junk,           // 20 %ld
                   &it_real,        // 21 %ld
                   &start,          // 22 UINTX_FORMAT
                   &vsize,          // 23 UINTX_FORMAT
                   &rss,            // 24 INTX_FORMAT
                   &rsslim,         // 25 UINTX_FORMAT
                   &scodes,         // 26 UINTX_FORMAT
                   &ecode,          // 27 UINTX_FORMAT
                   &stack_start);   // 28 UINTX_FORMAT
      }

#undef _UFM
#undef _DFM

      if (i != 28 - 2) {
        assert(false, "Bad conversion from /proc/self/stat");
        // product mode - assume we are the primordial thread, good luck in the
        // embedded case.
        warning("Can't detect primordial thread stack location - bad conversion");
        stack_start = (uintptr_t) &rlim;
      }
    } else {
      // For some reason we can't open /proc/self/stat (for example, running on
      // FreeBSD with a Linux emulator, or inside chroot), this should work for
      // most cases, so don't abort:
      warning("Can't detect primordial thread stack location - no /proc/self/stat");
      stack_start = (uintptr_t) &rlim;
    }
  }

  // Now we have a pointer (stack_start) very close to the stack top, the
  // next thing to do is to figure out the exact location of stack top. We
  // can find out the virtual memory area that contains stack_start by
  // reading /proc/self/maps, it should be the last vma in /proc/self/maps,
  // and its upper limit is the real stack top. (again, this would fail if
  // running inside chroot, because /proc may not exist.)

  uintptr_t stack_top;
  address low, high;
  if (find_vma((address)stack_start, &low, &high)) {
    // success, "high" is the true stack top. (ignore "low", because initial
    // thread stack grows on demand, its real bottom is high - RLIMIT_STACK.)
    stack_top = (uintptr_t)high;
  } else {
    // failed, likely because /proc/self/maps does not exist
    warning("Can't detect primordial thread stack location - find_vma failed");
    // best effort: stack_start is normally within a few pages below the real
    // stack top, use it as stack top, and reduce stack size so we won't put
    // guard page outside stack.
    stack_top = stack_start;
    stack_size -= 16 * page_size();
  }

  // stack_top could be partially down the page so align it
  stack_top = align_up(stack_top, page_size());

  // Allowed stack value is minimum of max_size and what we derived from rlimit
  if (max_size > 0) {
    _initial_thread_stack_size = MIN2(max_size, stack_size);
  } else {
    // Accept the rlimit max, but if stack is unlimited then it will be huge, so
    // clamp it at 8MB as we do on Solaris
    _initial_thread_stack_size = MIN2(stack_size, 8*M);
  }
  _initial_thread_stack_size = align_down(_initial_thread_stack_size, page_size());
  _initial_thread_stack_bottom = (address)stack_top - _initial_thread_stack_size;

  assert(_initial_thread_stack_bottom < (address)stack_top, "overflow!");

  if (log_is_enabled(Info, os, thread)) {
    // See if we seem to be on primordial process thread
    bool primordial = uintptr_t(&rlim) > uintptr_t(_initial_thread_stack_bottom) &&
                      uintptr_t(&rlim) < stack_top;

    log_info(os, thread)("Capturing initial stack in %s thread: req. size: " SIZE_FORMAT "K, actual size: "
                         SIZE_FORMAT "K, top=" INTPTR_FORMAT ", bottom=" INTPTR_FORMAT,
                         primordial ? "primordial" : "user", max_size / K,  _initial_thread_stack_size / K,
                         stack_top, intptr_t(_initial_thread_stack_bottom));
  }
}

////////////////////////////////////////////////////////////////////////////////
// time support

// Time since start-up in seconds to a fine granularity.
double os::elapsedTime() {
  return ((double)os::elapsed_counter()) / os::elapsed_frequency(); // nanosecond resolution
}

jlong os::elapsed_counter() {
  return javaTimeNanos() - initial_time_count;
}

jlong os::elapsed_frequency() {
  return NANOSECS_PER_SEC; // nanosecond resolution
}

bool os::supports_vtime() { return true; }

double os::elapsedVTime() {
  struct rusage usage;
  int retval = getrusage(RUSAGE_THREAD, &usage);
  if (retval == 0) {
    return (double) (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) + (double) (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / (1000 * 1000);
  } else {
    // better than nothing, but not much
    return elapsedTime();
  }
}

void os::Linux::fast_thread_clock_init() {
  if (!UseLinuxPosixThreadCPUClocks) {
    return;
  }
  clockid_t clockid;
  struct timespec tp;
  int (*pthread_getcpuclockid_func)(pthread_t, clockid_t *) =
      (int(*)(pthread_t, clockid_t *)) dlsym(RTLD_DEFAULT, "pthread_getcpuclockid");

  // Switch to using fast clocks for thread cpu time if
  // the clock_getres() returns 0 error code.
  // Note, that some kernels may support the current thread
  // clock (CLOCK_THREAD_CPUTIME_ID) but not the clocks
  // returned by the pthread_getcpuclockid().
  // If the fast POSIX clocks are supported then the clock_getres()
  // must return at least tp.tv_sec == 0 which means a resolution
  // better than 1 sec. This is extra check for reliability.

  if (pthread_getcpuclockid_func &&
      pthread_getcpuclockid_func(_main_thread, &clockid) == 0 &&
      clock_getres(clockid, &tp) == 0 && tp.tv_sec == 0) {
    _supports_fast_thread_cpu_time = true;
    _pthread_getcpuclockid = pthread_getcpuclockid_func;
  }
}

// Return the real, user, and system times in seconds from an
// arbitrary fixed point in the past.
bool os::getTimesSecs(double* process_real_time,
                      double* process_user_time,
                      double* process_system_time) {
  struct tms ticks;
  clock_t real_ticks = times(&ticks);

  if (real_ticks == (clock_t) (-1)) {
    return false;
  } else {
    double ticks_per_second = (double) clock_tics_per_sec;
    *process_user_time = ((double) ticks.tms_utime) / ticks_per_second;
    *process_system_time = ((double) ticks.tms_stime) / ticks_per_second;
    *process_real_time = ((double) real_ticks) / ticks_per_second;

    return true;
  }
}


char * os::local_time_string(char *buf, size_t buflen) {
  struct tm t;
  time_t long_time;
  time(&long_time);
  localtime_r(&long_time, &t);
  jio_snprintf(buf, buflen, "%d-%02d-%02d %02d:%02d:%02d",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
               t.tm_hour, t.tm_min, t.tm_sec);
  return buf;
}

struct tm* os::localtime_pd(const time_t* clock, struct tm*  res) {
  return localtime_r(clock, res);
}

// thread_id is kernel thread id (similar to Solaris LWP id)
intx os::current_thread_id() { return os::Linux::gettid(); }
int os::current_process_id() {
  return ::getpid();
}

// DLL functions

const char* os::dll_file_extension() { return ".so"; }

// This must be hard coded because it's the system's temporary
// directory not the java application's temp directory, ala java.io.tmpdir.
const char* os::get_temp_directory() { return "/tmp"; }

// check if addr is inside libjvm.so
bool os::address_is_in_vm(address addr) {
  static address libjvm_base_addr;
  Dl_info dlinfo;

  if (libjvm_base_addr == NULL) {
    if (dladdr(CAST_FROM_FN_PTR(void *, os::address_is_in_vm), &dlinfo) != 0) {
      libjvm_base_addr = (address)dlinfo.dli_fbase;
    }
    assert(libjvm_base_addr !=NULL, "Cannot obtain base address for libjvm");
  }

  if (dladdr((void *)addr, &dlinfo) != 0) {
    if (libjvm_base_addr == (address)dlinfo.dli_fbase) return true;
  }

  return false;
}

bool os::dll_address_to_function_name(address addr, char *buf,
                                      int buflen, int *offset,
                                      bool demangle) {
  // buf is not optional, but offset is optional
  assert(buf != NULL, "sanity check");

  Dl_info dlinfo;

  if (dladdr((void*)addr, &dlinfo) != 0) {
    // see if we have a matching symbol
    if (dlinfo.dli_saddr != NULL && dlinfo.dli_sname != NULL) {
      if (!(demangle && Decoder::demangle(dlinfo.dli_sname, buf, buflen))) {
        jio_snprintf(buf, buflen, "%s", dlinfo.dli_sname);
      }
      if (offset != NULL) *offset = addr - (address)dlinfo.dli_saddr;
      return true;
    }
    // no matching symbol so try for just file info
    if (dlinfo.dli_fname != NULL && dlinfo.dli_fbase != NULL) {
      if (Decoder::decode((address)(addr - (address)dlinfo.dli_fbase),
                          buf, buflen, offset, dlinfo.dli_fname, demangle)) {
        return true;
      }
    }
  }

  buf[0] = '\0';
  if (offset != NULL) *offset = -1;
  return false;
}

struct _address_to_library_name {
  address addr;          // input : memory address
  size_t  buflen;        //         size of fname
  char*   fname;         // output: library name
  address base;          //         library base addr
};

static int address_to_library_name_callback(struct dl_phdr_info *info,
                                            size_t size, void *data) {
  int i;
  bool found = false;
  address libbase = NULL;
  struct _address_to_library_name * d = (struct _address_to_library_name *)data;

  // iterate through all loadable segments
  for (i = 0; i < info->dlpi_phnum; i++) {
    address segbase = (address)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (info->dlpi_phdr[i].p_type == PT_LOAD) {
      // base address of a library is the lowest address of its loaded
      // segments.
      if (libbase == NULL || libbase > segbase) {
        libbase = segbase;
      }
      // see if 'addr' is within current segment
      if (segbase <= d->addr &&
          d->addr < segbase + info->dlpi_phdr[i].p_memsz) {
        found = true;
      }
    }
  }

  // dlpi_name is NULL or empty if the ELF file is executable, return 0
  // so dll_address_to_library_name() can fall through to use dladdr() which
  // can figure out executable name from argv[0].
  if (found && info->dlpi_name && info->dlpi_name[0]) {
    d->base = libbase;
    if (d->fname) {
      jio_snprintf(d->fname, d->buflen, "%s", info->dlpi_name);
    }
    return 1;
  }
  return 0;
}

bool os::dll_address_to_library_name(address addr, char* buf,
                                     int buflen, int* offset) {
  // buf is not optional, but offset is optional
  assert(buf != NULL, "sanity check");

  Dl_info dlinfo;
  struct _address_to_library_name data;

  // There is a bug in old glibc dladdr() implementation that it could resolve
  // to wrong library name if the .so file has a base address != NULL. Here
  // we iterate through the program headers of all loaded libraries to find
  // out which library 'addr' really belongs to. This workaround can be
  // removed once the minimum requirement for glibc is moved to 2.3.x.
  data.addr = addr;
  data.fname = buf;
  data.buflen = buflen;
  data.base = NULL;
  int rslt = dl_iterate_phdr(address_to_library_name_callback, (void *)&data);

  if (rslt) {
    // buf already contains library name
    if (offset) *offset = addr - data.base;
    return true;
  }
  if (dladdr((void*)addr, &dlinfo) != 0) {
    if (dlinfo.dli_fname != NULL) {
      jio_snprintf(buf, buflen, "%s", dlinfo.dli_fname);
    }
    if (dlinfo.dli_fbase != NULL && offset != NULL) {
      *offset = addr - (address)dlinfo.dli_fbase;
    }
    return true;
  }

  buf[0] = '\0';
  if (offset) *offset = -1;
  return false;
}

// Loads .dll/.so and
// in case of error it checks if .dll/.so was built for the
// same architecture as Hotspot is running on


// Remember the stack's state. The Linux dynamic linker will change
// the stack to 'executable' at most once, so we must safepoint only once.
bool os::Linux::_stack_is_executable = false;

// VM operation that loads a library.  This is necessary if stack protection
// of the Java stacks can be lost during loading the library.  If we
// do not stop the Java threads, they can stack overflow before the stacks
// are protected again.
class VM_LinuxDllLoad: public VM_Operation {
 private:
  const char *_filename;
  char *_ebuf;
  int _ebuflen;
  void *_lib;
 public:
  VM_LinuxDllLoad(const char *fn, char *ebuf, int ebuflen) :
    _filename(fn), _ebuf(ebuf), _ebuflen(ebuflen), _lib(NULL) {}
  VMOp_Type type() const { return VMOp_LinuxDllLoad; }
  void doit() {
    _lib = os::Linux::dll_load_in_vmthread(_filename, _ebuf, _ebuflen);
    os::Linux::_stack_is_executable = true;
  }
  void* loaded_library() { return _lib; }
};

void * os::dll_load(const char *filename, char *ebuf, int ebuflen) {
  void * result = NULL;
  bool load_attempted = false;

  log_info(os)("attempting shared library load of %s", filename);

  // Check whether the library to load might change execution rights
  // of the stack. If they are changed, the protection of the stack
  // guard pages will be lost. We need a safepoint to fix this.
  //
  // See Linux man page execstack(8) for more info.
  if (os::uses_stack_guard_pages() && !os::Linux::_stack_is_executable) {
    if (!ElfFile::specifies_noexecstack(filename)) {
      if (!is_init_completed()) {
        os::Linux::_stack_is_executable = true;
        // This is OK - No Java threads have been created yet, and hence no
        // stack guard pages to fix.
        //
        // Dynamic loader will make all stacks executable after
        // this function returns, and will not do that again.
        assert(Threads::number_of_threads() == 0, "no Java threads should exist yet.");
      } else {
        warning("You have loaded library %s which might have disabled stack guard. "
                "The VM will try to fix the stack guard now.\n"
                "It's highly recommended that you fix the library with "
                "'execstack -c <libfile>', or link it with '-z noexecstack'.",
                filename);

        JavaThread *jt = JavaThread::current();
        if (jt->thread_state() != _thread_in_native) {
          // This happens when a compiler thread tries to load a hsdis-<arch>.so file
          // that requires ExecStack. Cannot enter safe point. Let's give up.
          warning("Unable to fix stack guard. Giving up.");
        } else {
          if (!LoadExecStackDllInVMThread) {
            // This is for the case where the DLL has an static
            // constructor function that executes JNI code. We cannot
            // load such DLLs in the VMThread.
            result = os::Linux::dlopen_helper(filename, ebuf, ebuflen);
          }

          ThreadInVMfromNative tiv(jt);
          debug_only(VMNativeEntryWrapper vew;)

          VM_LinuxDllLoad op(filename, ebuf, ebuflen);
          VMThread::execute(&op);
          if (LoadExecStackDllInVMThread) {
            result = op.loaded_library();
          }
          load_attempted = true;
        }
      }
    }
  }

  if (!load_attempted) {
    result = os::Linux::dlopen_helper(filename, ebuf, ebuflen);
  }

  if (result != NULL) {
    // Successful loading
    return result;
  }

  Elf32_Ehdr elf_head;
  int diag_msg_max_length=ebuflen-strlen(ebuf);
  char* diag_msg_buf=ebuf+strlen(ebuf);

  if (diag_msg_max_length==0) {
    // No more space in ebuf for additional diagnostics message
    return NULL;
  }


  int file_descriptor= ::open(filename, O_RDONLY | O_NONBLOCK);

  if (file_descriptor < 0) {
    // Can't open library, report dlerror() message
    return NULL;
  }

  bool failed_to_read_elf_head=
    (sizeof(elf_head)!=
     (::read(file_descriptor, &elf_head,sizeof(elf_head))));

  ::close(file_descriptor);
  if (failed_to_read_elf_head) {
    // file i/o error - report dlerror() msg
    return NULL;
  }

  if (elf_head.e_ident[EI_DATA] != LITTLE_ENDIAN_ONLY(ELFDATA2LSB) BIG_ENDIAN_ONLY(ELFDATA2MSB)) {
    // handle invalid/out of range endianness values
    if (elf_head.e_ident[EI_DATA] == 0 || elf_head.e_ident[EI_DATA] > 2) {
      return NULL;
    }

#if defined(VM_LITTLE_ENDIAN)
    // VM is LE, shared object BE
    elf_head.e_machine = be16toh(elf_head.e_machine);
#else
    // VM is BE, shared object LE
    elf_head.e_machine = le16toh(elf_head.e_machine);
#endif
  }

  typedef struct {
    Elf32_Half    code;         // Actual value as defined in elf.h
    Elf32_Half    compat_class; // Compatibility of archs at VM's sense
    unsigned char elf_class;    // 32 or 64 bit
    unsigned char endianness;   // MSB or LSB
    char*         name;         // String representation
  } arch_t;

#ifndef EM_AARCH64
  #define EM_AARCH64    183               /* ARM AARCH64 */
#endif
#ifndef EM_RISCV
  #define EM_RISCV      243               /* RISC-V */
#endif
#ifndef EM_LOONGARCH
  #define EM_LOONGARCH  258               /* LoongArch */
#endif

  static const arch_t arch_array[]={
    {EM_386,         EM_386,     ELFCLASS32, ELFDATA2LSB, (char*)"IA 32"},
    {EM_486,         EM_386,     ELFCLASS32, ELFDATA2LSB, (char*)"IA 32"},
    {EM_IA_64,       EM_IA_64,   ELFCLASS64, ELFDATA2LSB, (char*)"IA 64"},
    {EM_X86_64,      EM_X86_64,  ELFCLASS64, ELFDATA2LSB, (char*)"AMD 64"},
    {EM_SPARC,       EM_SPARC,   ELFCLASS32, ELFDATA2MSB, (char*)"Sparc 32"},
    {EM_SPARC32PLUS, EM_SPARC,   ELFCLASS32, ELFDATA2MSB, (char*)"Sparc 32"},
    {EM_SPARCV9,     EM_SPARCV9, ELFCLASS64, ELFDATA2MSB, (char*)"Sparc v9 64"},
    {EM_PPC,         EM_PPC,     ELFCLASS32, ELFDATA2MSB, (char*)"Power PC 32"},
#if defined(VM_LITTLE_ENDIAN)
    {EM_PPC64,       EM_PPC64,   ELFCLASS64, ELFDATA2LSB, (char*)"Power PC 64 LE"},
    {EM_SH,          EM_SH,      ELFCLASS32, ELFDATA2LSB, (char*)"SuperH"},
#else
    {EM_PPC64,       EM_PPC64,   ELFCLASS64, ELFDATA2MSB, (char*)"Power PC 64"},
    {EM_SH,          EM_SH,      ELFCLASS32, ELFDATA2MSB, (char*)"SuperH BE"},
#endif
    {EM_ARM,         EM_ARM,     ELFCLASS32, ELFDATA2LSB, (char*)"ARM"},
    // we only support 64 bit z architecture
    {EM_S390,        EM_S390,    ELFCLASS64, ELFDATA2MSB, (char*)"IBM System/390"},
    {EM_ALPHA,       EM_ALPHA,   ELFCLASS64, ELFDATA2LSB, (char*)"Alpha"},
    {EM_MIPS_RS3_LE, EM_MIPS_RS3_LE, ELFCLASS32, ELFDATA2LSB, (char*)"MIPSel"},
    {EM_MIPS,        EM_MIPS,    ELFCLASS32, ELFDATA2MSB, (char*)"MIPS"},
    {EM_PARISC,      EM_PARISC,  ELFCLASS32, ELFDATA2MSB, (char*)"PARISC"},
    {EM_68K,         EM_68K,     ELFCLASS32, ELFDATA2MSB, (char*)"M68k"},
    {EM_AARCH64,     EM_AARCH64, ELFCLASS64, ELFDATA2LSB, (char*)"AARCH64"},
    {EM_RISCV,       EM_RISCV,   ELFCLASS64, ELFDATA2LSB, (char*)"RISC-V"},
    {EM_LOONGARCH,   EM_LOONGARCH, ELFCLASS64, ELFDATA2LSB, (char*)"LoongArch"},
  };

#if  (defined IA32)
  static  Elf32_Half running_arch_code=EM_386;
#elif   (defined AMD64) || (defined X32)
  static  Elf32_Half running_arch_code=EM_X86_64;
#elif  (defined IA64)
  static  Elf32_Half running_arch_code=EM_IA_64;
#elif  (defined __sparc) && (defined _LP64)
  static  Elf32_Half running_arch_code=EM_SPARCV9;
#elif  (defined __sparc) && (!defined _LP64)
  static  Elf32_Half running_arch_code=EM_SPARC;
#elif  (defined __powerpc64__)
  static  Elf32_Half running_arch_code=EM_PPC64;
#elif  (defined __powerpc__)
  static  Elf32_Half running_arch_code=EM_PPC;
#elif  (defined AARCH64)
  static  Elf32_Half running_arch_code=EM_AARCH64;
#elif  (defined ARM)
  static  Elf32_Half running_arch_code=EM_ARM;
#elif  (defined S390)
  static  Elf32_Half running_arch_code=EM_S390;
#elif  (defined ALPHA)
  static  Elf32_Half running_arch_code=EM_ALPHA;
#elif  (defined MIPSEL)
  static  Elf32_Half running_arch_code=EM_MIPS_RS3_LE;
#elif  (defined PARISC)
  static  Elf32_Half running_arch_code=EM_PARISC;
#elif  (defined MIPS)
  static  Elf32_Half running_arch_code=EM_MIPS;
#elif  (defined M68K)
  static  Elf32_Half running_arch_code=EM_68K;
#elif  (defined SH)
  static  Elf32_Half running_arch_code=EM_SH;
#elif  (defined RISCV)
  static  Elf32_Half running_arch_code=EM_RISCV;
#elif  (defined LOONGARCH64)
  static  Elf32_Half running_arch_code=EM_LOONGARCH;
#else
    #error Method os::dll_load requires that one of following is defined:\
        AARCH64, ALPHA, ARM, AMD64, IA32, IA64, LOONGARCH64, M68K, MIPS, MIPSEL, PARISC, __powerpc__, __powerpc64__, RISCV, S390, SH, __sparc
#endif

  // Identify compatibility class for VM's architecture and library's architecture
  // Obtain string descriptions for architectures

  arch_t lib_arch={elf_head.e_machine,0,elf_head.e_ident[EI_CLASS], elf_head.e_ident[EI_DATA], NULL};
  int running_arch_index=-1;

  for (unsigned int i=0; i < ARRAY_SIZE(arch_array); i++) {
    if (running_arch_code == arch_array[i].code) {
      running_arch_index    = i;
    }
    if (lib_arch.code == arch_array[i].code) {
      lib_arch.compat_class = arch_array[i].compat_class;
      lib_arch.name         = arch_array[i].name;
    }
  }

  assert(running_arch_index != -1,
         "Didn't find running architecture code (running_arch_code) in arch_array");
  if (running_arch_index == -1) {
    // Even though running architecture detection failed
    // we may still continue with reporting dlerror() message
    return NULL;
  }

  if (lib_arch.compat_class != arch_array[running_arch_index].compat_class) {
    if (lib_arch.name != NULL) {
      ::snprintf(diag_msg_buf, diag_msg_max_length-1,
                 " (Possible cause: can't load %s .so on a %s platform)",
                 lib_arch.name, arch_array[running_arch_index].name);
    } else {
      ::snprintf(diag_msg_buf, diag_msg_max_length-1,
                 " (Possible cause: can't load this .so (machine code=0x%x) on a %s platform)",
                 lib_arch.code, arch_array[running_arch_index].name);
    }
    return NULL;
  }

  if (lib_arch.endianness != arch_array[running_arch_index].endianness) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1, " (Possible cause: endianness mismatch)");
    return NULL;
  }

  // ELF file class/capacity : 0 - invalid, 1 - 32bit, 2 - 64bit
  if (lib_arch.elf_class > 2 || lib_arch.elf_class < 1) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1, " (Possible cause: invalid ELF file class)");
    return NULL;
  }

  if (lib_arch.elf_class != arch_array[running_arch_index].elf_class) {
    ::snprintf(diag_msg_buf, diag_msg_max_length-1,
               " (Possible cause: architecture word width mismatch, can't load %d-bit .so on a %d-bit platform)",
               (int) lib_arch.elf_class * 32, arch_array[running_arch_index].elf_class * 32);
    return NULL;
  }

  return NULL;
}

void * os::Linux::dlopen_helper(const char *filename, char *ebuf,
                                int ebuflen) {
  void * result = ::dlopen(filename, RTLD_LAZY);
  if (result == NULL) {
    const char* error_report = ::dlerror();
    if (error_report == NULL) {
      error_report = "dlerror returned no error description";
    }
    if (ebuf != NULL && ebuflen > 0) {
      ::strncpy(ebuf, error_report, ebuflen-1);
      ebuf[ebuflen-1]='\0';
    }
    Events::log_dll_message(NULL, "Loading shared library %s failed, %s", filename, error_report);
    log_info(os)("shared library load of %s failed, %s", filename, error_report);
  } else {
    Events::log_dll_message(NULL, "Loaded shared library %s", filename);
    log_info(os)("shared library load of %s was successful", filename);
  }
  return result;
}

void * os::Linux::dll_load_in_vmthread(const char *filename, char *ebuf,
                                       int ebuflen) {
  void * result = NULL;
  if (LoadExecStackDllInVMThread) {
    result = dlopen_helper(filename, ebuf, ebuflen);
  }

  // Since 7019808, libjvm.so is linked with -noexecstack. If the VM loads a
  // library that requires an executable stack, or which does not have this
  // stack attribute set, dlopen changes the stack attribute to executable. The
  // read protection of the guard pages gets lost.
  //
  // Need to check _stack_is_executable again as multiple VM_LinuxDllLoad
  // may have been queued at the same time.

  if (!_stack_is_executable) {
    for (JavaThreadIteratorWithHandle jtiwh; JavaThread *jt = jtiwh.next(); ) {
      StackOverflow* overflow_state = jt->stack_overflow_state();
      if (!overflow_state->stack_guard_zone_unused() &&     // Stack not yet fully initialized
          overflow_state->stack_guards_enabled()) {         // No pending stack overflow exceptions
        if (!os::guard_memory((char *)jt->stack_end(), StackOverflow::stack_guard_zone_size())) {
          warning("Attempt to reguard stack yellow zone failed.");
        }
      }
    }
  }

  return result;
}

const char* os::Linux::dll_path(void* lib) {
  struct link_map *lmap;
  const char* l_path = NULL;
  assert(lib != NULL, "dll_path parameter must not be NULL");

  int res_dli = ::dlinfo(lib, RTLD_DI_LINKMAP, &lmap);
  if (res_dli == 0) {
    l_path = lmap->l_name;
  }
  return l_path;
}

static bool _print_ascii_file(const char* filename, outputStream* st, const char* hdr = NULL) {
  int fd = ::open(filename, O_RDONLY);
  if (fd == -1) {
    return false;
  }

  if (hdr != NULL) {
    st->print_cr("%s", hdr);
  }

  char buf[33];
  int bytes;
  buf[32] = '\0';
  while ((bytes = ::read(fd, buf, sizeof(buf)-1)) > 0) {
    st->print_raw(buf, bytes);
  }

  ::close(fd);

  return true;
}

static void _print_ascii_file_h(const char* header, const char* filename, outputStream* st, bool same_line = true) {
  st->print("%s:%c", header, same_line ? ' ' : '\n');
  if (!_print_ascii_file(filename, st)) {
    st->print_cr("<Not Available>");
  }
}

void os::print_dll_info(outputStream *st) {
  st->print_cr("Dynamic libraries:");

  char fname[32];
  pid_t pid = os::Linux::gettid();

  jio_snprintf(fname, sizeof(fname), "/proc/%d/maps", pid);

  if (!_print_ascii_file(fname, st)) {
    st->print_cr("Can not get library information for pid = %d", pid);
  }
}

struct loaded_modules_info_param {
  os::LoadedModulesCallbackFunc callback;
  void *param;
};

static int dl_iterate_callback(struct dl_phdr_info *info, size_t size, void *data) {
  if ((info->dlpi_name == NULL) || (*info->dlpi_name == '\0')) {
    return 0;
  }

  struct loaded_modules_info_param *callback_param = reinterpret_cast<struct loaded_modules_info_param *>(data);
  address base = NULL;
  address top = NULL;
  for (int idx = 0; idx < info->dlpi_phnum; idx++) {
    const ElfW(Phdr) *phdr = info->dlpi_phdr + idx;
    if (phdr->p_type == PT_LOAD) {
      address raw_phdr_base = reinterpret_cast<address>(info->dlpi_addr + phdr->p_vaddr);

      address phdr_base = align_down(raw_phdr_base, phdr->p_align);
      if ((base == NULL) || (base > phdr_base)) {
        base = phdr_base;
      }

      address phdr_top = align_up(raw_phdr_base + phdr->p_memsz, phdr->p_align);
      if ((top == NULL) || (top < phdr_top)) {
        top = phdr_top;
      }
    }
  }

  return callback_param->callback(info->dlpi_name, base, top, callback_param->param);
}

int os::get_loaded_modules_info(os::LoadedModulesCallbackFunc callback, void *param) {
  struct loaded_modules_info_param callback_param = {callback, param};
  return dl_iterate_phdr(&dl_iterate_callback, &callback_param);
}

void os::print_os_info_brief(outputStream* st) {
  os::Linux::print_distro_info(st);

  os::Posix::print_uname_info(st);

  os::Linux::print_libversion_info(st);

}

void os::print_os_info(outputStream* st) {
  st->print_cr("OS:");

  os::Linux::print_distro_info(st);

  os::Posix::print_uname_info(st);

  os::Linux::print_uptime_info(st);

  // Print warning if unsafe chroot environment detected
  if (unsafe_chroot_detected) {
    st->print_cr("WARNING!! %s", unstable_chroot_error);
  }

  os::Linux::print_libversion_info(st);

  os::Posix::print_rlimit_info(st);

  os::Posix::print_load_average(st);
  st->cr();

  os::Linux::print_system_memory_info(st);
  st->cr();

  os::Linux::print_process_memory_info(st);
  st->cr();

  os::Linux::print_proc_sys_info(st);
  st->cr();

  if (os::Linux::print_ld_preload_file(st)) {
    st->cr();
  }

  if (os::Linux::print_container_info(st)) {
    st->cr();
  }

  VM_Version::print_platform_virtualization_info(st);

  os::Linux::print_steal_info(st);
}

// Try to identify popular distros.
// Most Linux distributions have a /etc/XXX-release file, which contains
// the OS version string. Newer Linux distributions have a /etc/lsb-release
// file that also contains the OS version string. Some have more than one
// /etc/XXX-release file (e.g. Mandrake has both /etc/mandrake-release and
// /etc/redhat-release.), so the order is important.
// Any Linux that is based on Redhat (i.e. Oracle, Mandrake, Sun JDS...) have
// their own specific XXX-release file as well as a redhat-release file.
// Because of this the XXX-release file needs to be searched for before the
// redhat-release file.
// Since Red Hat and SuSE have an lsb-release file that is not very descriptive the
// search for redhat-release / SuSE-release needs to be before lsb-release.
// Since the lsb-release file is the new standard it needs to be searched
// before the older style release files.
// Searching system-release (Red Hat) and os-release (other Linuxes) are a
// next to last resort.  The os-release file is a new standard that contains
// distribution information and the system-release file seems to be an old
// standard that has been replaced by the lsb-release and os-release files.
// Searching for the debian_version file is the last resort.  It contains
// an informative string like "6.0.6" or "wheezy/sid". Because of this
// "Debian " is printed before the contents of the debian_version file.

const char* distro_files[] = {
  "/etc/oracle-release",
  "/etc/mandriva-release",
  "/etc/mandrake-release",
  "/etc/sun-release",
  "/etc/redhat-release",
  "/etc/lsb-release",
  "/etc/turbolinux-release",
  "/etc/gentoo-release",
  "/etc/ltib-release",
  "/etc/angstrom-version",
  "/etc/system-release",
  "/etc/os-release",
  "/etc/SuSE-release", // Deprecated in favor of os-release since SuSE 12
  NULL };

void os::Linux::print_distro_info(outputStream* st) {
  for (int i = 0;; i++) {
    const char* file = distro_files[i];
    if (file == NULL) {
      break;  // done
    }
    // If file prints, we found it.
    if (_print_ascii_file(file, st)) {
      return;
    }
  }

  if (file_exists("/etc/debian_version")) {
    st->print("Debian ");
    _print_ascii_file("/etc/debian_version", st);
  } else {
    st->print_cr("Linux");
  }
}

static void parse_os_info_helper(FILE* fp, char* distro, size_t length, bool get_first_line) {
  char buf[256];
  while (fgets(buf, sizeof(buf), fp)) {
    // Edit out extra stuff in expected format
    if (strstr(buf, "DISTRIB_DESCRIPTION=") != NULL || strstr(buf, "PRETTY_NAME=") != NULL) {
      char* ptr = strstr(buf, "\"");  // the name is in quotes
      if (ptr != NULL) {
        ptr++; // go beyond first quote
        char* nl = strchr(ptr, '\"');
        if (nl != NULL) *nl = '\0';
        strncpy(distro, ptr, length);
      } else {
        ptr = strstr(buf, "=");
        ptr++; // go beyond equals then
        char* nl = strchr(ptr, '\n');
        if (nl != NULL) *nl = '\0';
        strncpy(distro, ptr, length);
      }
      return;
    } else if (get_first_line) {
      char* nl = strchr(buf, '\n');
      if (nl != NULL) *nl = '\0';
      strncpy(distro, buf, length);
      return;
    }
  }
  // print last line and close
  char* nl = strchr(buf, '\n');
  if (nl != NULL) *nl = '\0';
  strncpy(distro, buf, length);
}

static void parse_os_info(char* distro, size_t length, const char* file) {
  FILE* fp = fopen(file, "r");
  if (fp != NULL) {
    // if suse format, print out first line
    bool get_first_line = (strcmp(file, "/etc/SuSE-release") == 0);
    parse_os_info_helper(fp, distro, length, get_first_line);
    fclose(fp);
  }
}

void os::get_summary_os_info(char* buf, size_t buflen) {
  for (int i = 0;; i++) {
    const char* file = distro_files[i];
    if (file == NULL) {
      break; // ran out of distro_files
    }
    if (file_exists(file)) {
      parse_os_info(buf, buflen, file);
      return;
    }
  }
  // special case for debian
  if (file_exists("/etc/debian_version")) {
    strncpy(buf, "Debian ", buflen);
    if (buflen > 7) {
      parse_os_info(&buf[7], buflen-7, "/etc/debian_version");
    }
  } else {
    strncpy(buf, "Linux", buflen);
  }
}

void os::Linux::print_libversion_info(outputStream* st) {
  // libc, pthread
  st->print("libc: ");
  st->print("%s ", os::Linux::libc_version());
  st->print("%s ", os::Linux::libpthread_version());
  st->cr();
}

void os::Linux::print_proc_sys_info(outputStream* st) {
  _print_ascii_file_h("/proc/sys/kernel/threads-max (system-wide limit on the number of threads)",
                      "/proc/sys/kernel/threads-max", st);
  _print_ascii_file_h("/proc/sys/vm/max_map_count (maximum number of memory map areas a process may have)",
                      "/proc/sys/vm/max_map_count", st);
  _print_ascii_file_h("/proc/sys/vm/swappiness (control to define how aggressively the kernel swaps out anonymous memory)",
                      "/proc/sys/vm/swappiness", st);
  _print_ascii_file_h("/proc/sys/kernel/pid_max (system-wide limit on number of process identifiers)",
                      "/proc/sys/kernel/pid_max", st);
}

void os::Linux::print_system_memory_info(outputStream* st) {
  _print_ascii_file_h("/proc/meminfo", "/proc/meminfo", st, false);
  st->cr();

  // some information regarding THPs; for details see
  // https://www.kernel.org/doc/Documentation/vm/transhuge.txt
  _print_ascii_file_h("/sys/kernel/mm/transparent_hugepage/enabled",
                      "/sys/kernel/mm/transparent_hugepage/enabled", st);
  _print_ascii_file_h("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size",
                      "/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", st);
  _print_ascii_file_h("/sys/kernel/mm/transparent_hugepage/defrag (defrag/compaction efforts parameter)",
                      "/sys/kernel/mm/transparent_hugepage/defrag", st);
}

bool os::Linux::query_process_memory_info(os::Linux::meminfo_t* info) {
  FILE* f = ::fopen("/proc/self/status", "r");
  const int num_values = sizeof(os::Linux::meminfo_t) / sizeof(size_t);
  int num_found = 0;
  char buf[256];
  info->vmsize = info->vmpeak = info->vmrss = info->vmhwm = info->vmswap =
      info->rssanon = info->rssfile = info->rssshmem = -1;
  if (f != NULL) {
    while (::fgets(buf, sizeof(buf), f) != NULL && num_found < num_values) {
      if ( (info->vmsize == -1    && sscanf(buf, "VmSize: " SSIZE_FORMAT " kB", &info->vmsize) == 1) ||
           (info->vmpeak == -1    && sscanf(buf, "VmPeak: " SSIZE_FORMAT " kB", &info->vmpeak) == 1) ||
           (info->vmswap == -1    && sscanf(buf, "VmSwap: " SSIZE_FORMAT " kB", &info->vmswap) == 1) ||
           (info->vmhwm == -1     && sscanf(buf, "VmHWM: " SSIZE_FORMAT " kB", &info->vmhwm) == 1) ||
           (info->vmrss == -1     && sscanf(buf, "VmRSS: " SSIZE_FORMAT " kB", &info->vmrss) == 1) ||
           (info->rssanon == -1   && sscanf(buf, "RssAnon: " SSIZE_FORMAT " kB", &info->rssanon) == 1) || // Needs Linux 4.5
           (info->rssfile == -1   && sscanf(buf, "RssFile: " SSIZE_FORMAT " kB", &info->rssfile) == 1) || // Needs Linux 4.5
           (info->rssshmem == -1  && sscanf(buf, "RssShmem: " SSIZE_FORMAT " kB", &info->rssshmem) == 1)  // Needs Linux 4.5
           )
      {
        num_found ++;
      }
    }
    fclose(f);
    return true;
  }
  return false;
}

#ifdef __GLIBC__
// For Glibc, print a one-liner with the malloc tunables.
// Most important and popular is MALLOC_ARENA_MAX, but we are
// thorough and print them all.
static void print_glibc_malloc_tunables(outputStream* st) {
  static const char* var[] = {
      // the new variant
      "GLIBC_TUNABLES",
      // legacy variants
      "MALLOC_CHECK_", "MALLOC_TOP_PAD_", "MALLOC_PERTURB_",
      "MALLOC_MMAP_THRESHOLD_", "MALLOC_TRIM_THRESHOLD_",
      "MALLOC_MMAP_MAX_", "MALLOC_ARENA_TEST", "MALLOC_ARENA_MAX",
      NULL};
  st->print("glibc malloc tunables: ");
  bool printed = false;
  for (int i = 0; var[i] != NULL; i ++) {
    const char* const val = ::getenv(var[i]);
    if (val != NULL) {
      st->print("%s%s=%s", (printed ? ", " : ""), var[i], val);
      printed = true;
    }
  }
  if (!printed) {
    st->print("(default)");
  }
}
#endif // __GLIBC__

void os::Linux::print_process_memory_info(outputStream* st) {

  st->print_cr("Process Memory:");

  // Print virtual and resident set size; peak values; swap; and for
  //  rss its components if the kernel is recent enough.
  meminfo_t info;
  if (query_process_memory_info(&info)) {
    st->print_cr("Virtual Size: " SSIZE_FORMAT "K (peak: " SSIZE_FORMAT "K)", info.vmsize, info.vmpeak);
    st->print("Resident Set Size: " SSIZE_FORMAT "K (peak: " SSIZE_FORMAT "K)", info.vmrss, info.vmhwm);
    if (info.rssanon != -1) { // requires kernel >= 4.5
      st->print(" (anon: " SSIZE_FORMAT "K, file: " SSIZE_FORMAT "K, shmem: " SSIZE_FORMAT "K)",
                info.rssanon, info.rssfile, info.rssshmem);
    }
    st->cr();
    if (info.vmswap != -1) { // requires kernel >= 2.6.34
      st->print_cr("Swapped out: " SSIZE_FORMAT "K", info.vmswap);
    }
  } else {
    st->print_cr("Could not open /proc/self/status to get process memory related information");
  }

  // glibc only:
  // - Print outstanding allocations using mallinfo
  // - Print glibc tunables
#ifdef __GLIBC__
  size_t total_allocated = 0;
  size_t free_retained = 0;
  bool might_have_wrapped = false;
  if (_mallinfo2 != NULL) {
    struct glibc_mallinfo2 mi = _mallinfo2();
    total_allocated = mi.uordblks + mi.hblkhd;
    free_retained = mi.fordblks;
  } else if (_mallinfo != NULL) {
    // mallinfo is an old API. Member names mean next to nothing and, beyond that, are 32-bit signed.
    // So for larger footprints the values may have wrapped around. We try to detect this here: if the
    // process whole resident set size is smaller than 4G, malloc footprint has to be less than that
    // and the numbers are reliable.
    struct glibc_mallinfo mi = _mallinfo();
    total_allocated = (size_t)(unsigned)mi.uordblks + (size_t)(unsigned)mi.hblkhd;
    free_retained = (size_t)(unsigned)mi.fordblks;
    // Since mallinfo members are int, glibc values may have wrapped. Warn about this.
    might_have_wrapped = (info.vmrss * K) > UINT_MAX && (info.vmrss * K) > (total_allocated + UINT_MAX);
  }
  if (_mallinfo2 != NULL || _mallinfo != NULL) {
    st->print_cr("C-Heap outstanding allocations: " SIZE_FORMAT "K, retained: " SIZE_FORMAT "K%s",
                 total_allocated / K, free_retained / K,
                 might_have_wrapped ? " (may have wrapped)" : "");
  }
  // Tunables
  print_glibc_malloc_tunables(st);
  st->cr();
#endif
}

bool os::Linux::print_ld_preload_file(outputStream* st) {
  return _print_ascii_file("/etc/ld.so.preload", st, "/etc/ld.so.preload:");
}

void os::Linux::print_uptime_info(outputStream* st) {
  struct sysinfo sinfo;
  int ret = sysinfo(&sinfo);
  if (ret == 0) {
    os::print_dhm(st, "OS uptime:", (long) sinfo.uptime);
  }
}

bool os::Linux::print_container_info(outputStream* st) {
  if (!OSContainer::is_containerized()) {
    st->print_cr("container information not found.");
    return false;
  }

  st->print_cr("container (cgroup) information:");

  const char *p_ct = OSContainer::container_type();
  st->print_cr("container_type: %s", p_ct != NULL ? p_ct : "not supported");

  char *p = OSContainer::cpu_cpuset_cpus();
  st->print_cr("cpu_cpuset_cpus: %s", p != NULL ? p : "not supported");
  free(p);

  p = OSContainer::cpu_cpuset_memory_nodes();
  st->print_cr("cpu_memory_nodes: %s", p != NULL ? p : "not supported");
  free(p);

  int i = OSContainer::active_processor_count();
  st->print("active_processor_count: ");
  if (i > 0) {
    if (ActiveProcessorCount > 0) {
      st->print_cr("%d, but overridden by -XX:ActiveProcessorCount %d", i, ActiveProcessorCount);
    } else {
      st->print_cr("%d", i);
    }
  } else {
    st->print_cr("not supported");
  }

  i = OSContainer::cpu_quota();
  st->print("cpu_quota: ");
  if (i > 0) {
    st->print_cr("%d", i);
  } else {
    st->print_cr("%s", i == OSCONTAINER_ERROR ? "not supported" : "no quota");
  }

  i = OSContainer::cpu_period();
  st->print("cpu_period: ");
  if (i > 0) {
    st->print_cr("%d", i);
  } else {
    st->print_cr("%s", i == OSCONTAINER_ERROR ? "not supported" : "no period");
  }

  i = OSContainer::cpu_shares();
  st->print("cpu_shares: ");
  if (i > 0) {
    st->print_cr("%d", i);
  } else {
    st->print_cr("%s", i == OSCONTAINER_ERROR ? "not supported" : "no shares");
  }

  OSContainer::print_container_helper(st, OSContainer::memory_limit_in_bytes(), "memory_limit_in_bytes");
  OSContainer::print_container_helper(st, OSContainer::memory_and_swap_limit_in_bytes(), "memory_and_swap_limit_in_bytes");
  OSContainer::print_container_helper(st, OSContainer::memory_soft_limit_in_bytes(), "memory_soft_limit_in_bytes");
  OSContainer::print_container_helper(st, OSContainer::memory_usage_in_bytes(), "memory_usage_in_bytes");
  OSContainer::print_container_helper(st, OSContainer::memory_max_usage_in_bytes(), "memory_max_usage_in_bytes");

  OSContainer::print_version_specific_info(st);

  jlong j = OSContainer::pids_max();
  st->print("maximum number of tasks: ");
  if (j > 0) {
    st->print_cr(JLONG_FORMAT, j);
  } else {
    st->print_cr("%s", j == OSCONTAINER_ERROR ? "not supported" : "unlimited");
  }

  j = OSContainer::pids_current();
  st->print("current number of tasks: ");
  if (j > 0) {
    st->print_cr(JLONG_FORMAT, j);
  } else {
    if (j == OSCONTAINER_ERROR) {
      st->print_cr("not supported");
    }
  }

  return true;
}

void os::Linux::print_steal_info(outputStream* st) {
  if (has_initial_tick_info) {
    CPUPerfTicks pticks;
    bool res = os::Linux::get_tick_information(&pticks, -1);

    if (res && pticks.has_steal_ticks) {
      uint64_t steal_ticks_difference = pticks.steal - initial_steal_ticks;
      uint64_t total_ticks_difference = pticks.total - initial_total_ticks;
      double steal_ticks_perc = 0.0;
      if (total_ticks_difference != 0) {
        steal_ticks_perc = (double) steal_ticks_difference / total_ticks_difference;
      }
      st->print_cr("Steal ticks since vm start: " UINT64_FORMAT, steal_ticks_difference);
      st->print_cr("Steal ticks percentage since vm start:%7.3f", steal_ticks_perc);
    }
  }
}

void os::print_memory_info(outputStream* st) {

  st->print("Memory:");
  st->print(" %dk page", os::vm_page_size()>>10);

  // values in struct sysinfo are "unsigned long"
  struct sysinfo si;
  sysinfo(&si);

  st->print(", physical " UINT64_FORMAT "k",
            os::physical_memory() >> 10);
  st->print("(" UINT64_FORMAT "k free)",
            os::available_memory() >> 10);
  st->print(", swap " UINT64_FORMAT "k",
            ((jlong)si.totalswap * si.mem_unit) >> 10);
  st->print("(" UINT64_FORMAT "k free)",
            ((jlong)si.freeswap * si.mem_unit) >> 10);
  st->cr();
  st->print("Page Sizes: ");
  _page_sizes.print_on(st);
  st->cr();
}

// Print the first "model name" line and the first "flags" line
// that we find and nothing more. We assume "model name" comes
// before "flags" so if we find a second "model name", then the
// "flags" field is considered missing.
static bool print_model_name_and_flags(outputStream* st, char* buf, size_t buflen) {
#if defined(IA32) || defined(AMD64)
  // Other platforms have less repetitive cpuinfo files
  FILE *fp = fopen("/proc/cpuinfo", "r");
  if (fp) {
    bool model_name_printed = false;
    while (!feof(fp)) {
      if (fgets(buf, buflen, fp)) {
        // Assume model name comes before flags
        if (strstr(buf, "model name") != NULL) {
          if (!model_name_printed) {
            st->print_raw("CPU Model and flags from /proc/cpuinfo:\n");
            st->print_raw(buf);
            model_name_printed = true;
          } else {
            // model name printed but not flags?  Odd, just return
            fclose(fp);
            return true;
          }
        }
        // print the flags line too
        if (strstr(buf, "flags") != NULL) {
          st->print_raw(buf);
          fclose(fp);
          return true;
        }
      }
    }
    fclose(fp);
  }
#endif // x86 platforms
  return false;
}

// additional information about CPU e.g. available frequency ranges
static void print_sys_devices_cpu_info(outputStream* st, char* buf, size_t buflen) {
  _print_ascii_file_h("Online cpus", "/sys/devices/system/cpu/online", st);
  _print_ascii_file_h("Offline cpus", "/sys/devices/system/cpu/offline", st);

  if (ExtensiveErrorReports) {
    // cache related info (cpu 0, should be similar for other CPUs)
    for (unsigned int i=0; i < 10; i++) { // handle max. 10 cache entries
      char hbuf_level[60];
      char hbuf_type[60];
      char hbuf_size[60];
      char hbuf_coherency_line_size[80];
      snprintf(hbuf_level, 60, "/sys/devices/system/cpu/cpu0/cache/index%u/level", i);
      snprintf(hbuf_type, 60, "/sys/devices/system/cpu/cpu0/cache/index%u/type", i);
      snprintf(hbuf_size, 60, "/sys/devices/system/cpu/cpu0/cache/index%u/size", i);
      snprintf(hbuf_coherency_line_size, 80, "/sys/devices/system/cpu/cpu0/cache/index%u/coherency_line_size", i);
      if (os::file_exists(hbuf_level)) {
        _print_ascii_file_h("cache level", hbuf_level, st);
        _print_ascii_file_h("cache type", hbuf_type, st);
        _print_ascii_file_h("cache size", hbuf_size, st);
        _print_ascii_file_h("cache coherency line size", hbuf_coherency_line_size, st);
      }
    }
  }

  // we miss the cpufreq entries on Power and s390x
#if defined(IA32) || defined(AMD64)
  _print_ascii_file_h("BIOS frequency limitation", "/sys/devices/system/cpu/cpu0/cpufreq/bios_limit", st);
  _print_ascii_file_h("Frequency switch latency (ns)", "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_transition_latency", st);
  _print_ascii_file_h("Available cpu frequencies", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies", st);
  // min and max should be in the Available range but still print them (not all info might be available for all kernels)
  if (ExtensiveErrorReports) {
    _print_ascii_file_h("Maximum cpu frequency", "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", st);
    _print_ascii_file_h("Minimum cpu frequency", "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", st);
    _print_ascii_file_h("Current cpu frequency", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", st);
  }
  // governors are power schemes, see https://wiki.archlinux.org/index.php/CPU_frequency_scaling
  if (ExtensiveErrorReports) {
    _print_ascii_file_h("Available governors", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors", st);
  }
  _print_ascii_file_h("Current governor", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", st);
  // Core performance boost, see https://www.kernel.org/doc/Documentation/cpu-freq/boost.txt
  // Raise operating frequency of some cores in a multi-core package if certain conditions apply, e.g.
  // whole chip is not fully utilized
  _print_ascii_file_h("Core performance/turbo boost", "/sys/devices/system/cpu/cpufreq/boost", st);
#endif
}

void os::pd_print_cpu_info(outputStream* st, char* buf, size_t buflen) {
  // Only print the model name if the platform provides this as a summary
  if (!print_model_name_and_flags(st, buf, buflen)) {
    _print_ascii_file_h("/proc/cpuinfo", "/proc/cpuinfo", st, false);
  }
  st->cr();
  print_sys_devices_cpu_info(st, buf, buflen);
}

#if defined(AMD64) || defined(IA32) || defined(X32)
const char* search_string = "model name";
#elif defined(M68K)
const char* search_string = "CPU";
#elif defined(PPC64)
const char* search_string = "cpu";
#elif defined(S390)
const char* search_string = "machine =";
#elif defined(SPARC)
const char* search_string = "cpu";
#else
const char* search_string = "Processor";
#endif

// Parses the cpuinfo file for string representing the model name.
void os::get_summary_cpu_info(char* cpuinfo, size_t length) {
  FILE* fp = fopen("/proc/cpuinfo", "r");
  if (fp != NULL) {
    while (!feof(fp)) {
      char buf[256];
      if (fgets(buf, sizeof(buf), fp)) {
        char* start = strstr(buf, search_string);
        if (start != NULL) {
          char *ptr = start + strlen(search_string);
          char *end = buf + strlen(buf);
          while (ptr != end) {
             // skip whitespace and colon for the rest of the name.
             if (*ptr != ' ' && *ptr != '\t' && *ptr != ':') {
               break;
             }
             ptr++;
          }
          if (ptr != end) {
            // reasonable string, get rid of newline and keep the rest
            char* nl = strchr(buf, '\n');
            if (nl != NULL) *nl = '\0';
            strncpy(cpuinfo, ptr, length);
            fclose(fp);
            return;
          }
        }
      }
    }
    fclose(fp);
  }
  // cpuinfo not found or parsing failed, just print generic string.  The entire
  // /proc/cpuinfo file will be printed later in the file (or enough of it for x86)
#if   defined(AARCH64)
  strncpy(cpuinfo, "AArch64", length);
#elif defined(AMD64)
  strncpy(cpuinfo, "x86_64", length);
#elif defined(ARM)  // Order wrt. AARCH64 is relevant!
  strncpy(cpuinfo, "ARM", length);
#elif defined(IA32)
  strncpy(cpuinfo, "x86_32", length);
#elif defined(IA64)
  strncpy(cpuinfo, "IA64", length);
#elif defined(PPC)
  strncpy(cpuinfo, "PPC64", length);
#elif defined(RISCV)
  strncpy(cpuinfo, "RISCV64", length);
#elif defined(S390)
  strncpy(cpuinfo, "S390", length);
#elif defined(SPARC)
  strncpy(cpuinfo, "sparcv9", length);
#elif defined(ZERO_LIBARCH)
  strncpy(cpuinfo, ZERO_LIBARCH, length);
#else
  strncpy(cpuinfo, "unknown", length);
#endif
}

static char saved_jvm_path[MAXPATHLEN] = {0};

// Find the full path to the current module, libjvm.so
void os::jvm_path(char *buf, jint buflen) {
  // Error checking.
  if (buflen < MAXPATHLEN) {
    assert(false, "must use a large-enough buffer");
    buf[0] = '\0';
    return;
  }
  // Lazy resolve the path to current module.
  if (saved_jvm_path[0] != 0) {
    strcpy(buf, saved_jvm_path);
    return;
  }

  char dli_fname[MAXPATHLEN];
  dli_fname[0] = '\0';
  bool ret = dll_address_to_library_name(
                                         CAST_FROM_FN_PTR(address, os::jvm_path),
                                         dli_fname, sizeof(dli_fname), NULL);
  assert(ret, "cannot locate libjvm");
  char *rp = NULL;
  if (ret && dli_fname[0] != '\0') {
    rp = os::Posix::realpath(dli_fname, buf, buflen);
  }
  if (rp == NULL) {
    return;
  }

  if (Arguments::sun_java_launcher_is_altjvm()) {
    // Support for the java launcher's '-XXaltjvm=<path>' option. Typical
    // value for buf is "<JAVA_HOME>/jre/lib/<vmtype>/libjvm.so".
    // If "/jre/lib/" appears at the right place in the string, then
    // assume we are installed in a JDK and we're done. Otherwise, check
    // for a JAVA_HOME environment variable and fix up the path so it
    // looks like libjvm.so is installed there (append a fake suffix
    // hotspot/libjvm.so).
    const char *p = buf + strlen(buf) - 1;
    for (int count = 0; p > buf && count < 5; ++count) {
      for (--p; p > buf && *p != '/'; --p)
        /* empty */ ;
    }

    if (strncmp(p, "/jre/lib/", 9) != 0) {
      // Look for JAVA_HOME in the environment.
      char* java_home_var = ::getenv("JAVA_HOME");
      if (java_home_var != NULL && java_home_var[0] != 0) {
        char* jrelib_p;
        int len;

        // Check the current module name "libjvm.so".
        p = strrchr(buf, '/');
        if (p == NULL) {
          return;
        }
        assert(strstr(p, "/libjvm") == p, "invalid library name");

        rp = os::Posix::realpath(java_home_var, buf, buflen);
        if (rp == NULL) {
          return;
        }

        // determine if this is a legacy image or modules image
        // modules image doesn't have "jre" subdirectory
        len = strlen(buf);
        assert(len < buflen, "Ran out of buffer room");
        jrelib_p = buf + len;
        snprintf(jrelib_p, buflen-len, "/jre/lib");
        if (0 != access(buf, F_OK)) {
          snprintf(jrelib_p, buflen-len, "/lib");
        }

        if (0 == access(buf, F_OK)) {
          // Use current module name "libjvm.so"
          len = strlen(buf);
          snprintf(buf + len, buflen-len, "/hotspot/libjvm.so");
        } else {
          // Go back to path of .so
          rp = os::Posix::realpath(dli_fname, buf, buflen);
          if (rp == NULL) {
            return;
          }
        }
      }
    }
  }

  strncpy(saved_jvm_path, buf, MAXPATHLEN);
  saved_jvm_path[MAXPATHLEN - 1] = '\0';
}

void os::print_jni_name_prefix_on(outputStream* st, int args_size) {
  // no prefix required, not even "_"
}

void os::print_jni_name_suffix_on(outputStream* st, int args_size) {
  // no suffix required
}

////////////////////////////////////////////////////////////////////////////////
// Virtual Memory

int os::vm_page_size() {
  // Seems redundant as all get out
  assert(os::Linux::page_size() != -1, "must call os::init");
  return os::Linux::page_size();
}

// Solaris allocates memory by pages.
int os::vm_allocation_granularity() {
  assert(os::Linux::page_size() != -1, "must call os::init");
  return os::Linux::page_size();
}

// Rationale behind this function:
//  current (Mon Apr 25 20:12:18 MSD 2005) oprofile drops samples without executable
//  mapping for address (see lookup_dcookie() in the kernel module), thus we cannot get
//  samples for JITted code. Here we create private executable mapping over the code cache
//  and then we can use standard (well, almost, as mapping can change) way to provide
//  info for the reporting script by storing timestamp and location of symbol
void linux_wrap_code(char* base, size_t size) {
  static volatile jint cnt = 0;

  if (!UseOprofile) {
    return;
  }

  char buf[PATH_MAX+1];
  int num = Atomic::add(&cnt, 1);

  snprintf(buf, sizeof(buf), "%s/hs-vm-%d-%d",
           os::get_temp_directory(), os::current_process_id(), num);
  unlink(buf);

  int fd = ::open(buf, O_CREAT | O_RDWR, S_IRWXU);

  if (fd != -1) {
    off_t rv = ::lseek(fd, size-2, SEEK_SET);
    if (rv != (off_t)-1) {
      if (::write(fd, "", 1) == 1) {
        mmap(base, size,
             PROT_READ|PROT_WRITE|PROT_EXEC,
             MAP_PRIVATE|MAP_FIXED|MAP_NORESERVE, fd, 0);
      }
    }
    ::close(fd);
    unlink(buf);
  }
}

static bool recoverable_mmap_error(int err) {
  // See if the error is one we can let the caller handle. This
  // list of errno values comes from JBS-6843484. I can't find a
  // Linux man page that documents this specific set of errno
  // values so while this list currently matches Solaris, it may
  // change as we gain experience with this failure mode.
  switch (err) {
  case EBADF:
  case EINVAL:
  case ENOTSUP:
    // let the caller deal with these errors
    return true;

  default:
    // Any remaining errors on this OS can cause our reserved mapping
    // to be lost. That can cause confusion where different data
    // structures think they have the same memory mapped. The worst
    // scenario is if both the VM and a library think they have the
    // same memory mapped.
    return false;
  }
}

static void warn_fail_commit_memory(char* addr, size_t size, bool exec,
                                    int err) {
  warning("INFO: os::commit_memory(" PTR_FORMAT ", " SIZE_FORMAT
          ", %d) failed; error='%s' (errno=%d)", p2i(addr), size, exec,
          os::strerror(err), err);
}

static void warn_fail_commit_memory(char* addr, size_t size,
                                    size_t alignment_hint, bool exec,
                                    int err) {
  warning("INFO: os::commit_memory(" PTR_FORMAT ", " SIZE_FORMAT
          ", " SIZE_FORMAT ", %d) failed; error='%s' (errno=%d)", p2i(addr), size,
          alignment_hint, exec, os::strerror(err), err);
}

// NOTE: Linux kernel does not really reserve the pages for us.
//       All it does is to check if there are enough free pages
//       left at the time of mmap(). This could be a potential
//       problem.
int os::Linux::commit_memory_impl(char* addr, size_t size, bool exec) {
  int prot = exec ? PROT_READ|PROT_WRITE|PROT_EXEC : PROT_READ|PROT_WRITE;
  uintptr_t res = (uintptr_t) ::mmap(addr, size, prot,
                                     MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
  if (res != (uintptr_t) MAP_FAILED) {
    if (UseNUMAInterleaving) {
      numa_make_global(addr, size);
    }
    return 0;
  }

  int err = errno;  // save errno from mmap() call above

  if (!recoverable_mmap_error(err)) {
    warn_fail_commit_memory(addr, size, exec, err);
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "committing reserved memory.");
  }

  return err;
}

bool os::pd_commit_memory(char* addr, size_t size, bool exec) {
  return os::Linux::commit_memory_impl(addr, size, exec) == 0;
}

void os::pd_commit_memory_or_exit(char* addr, size_t size, bool exec,
                                  const char* mesg) {
  assert(mesg != NULL, "mesg must be specified");
  int err = os::Linux::commit_memory_impl(addr, size, exec);
  if (err != 0) {
    // the caller wants all commit errors to exit with the specified mesg:
    warn_fail_commit_memory(addr, size, exec, err);
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "%s", mesg);
  }
}

// Define MAP_HUGETLB here so we can build HotSpot on old systems.
#ifndef MAP_HUGETLB
  #define MAP_HUGETLB 0x40000
#endif

// If mmap flags are set with MAP_HUGETLB and the system supports multiple
// huge page sizes, flag bits [26:31] can be used to encode the log2 of the
// desired huge page size. Otherwise, the system's default huge page size will be used.
// See mmap(2) man page for more info (since Linux 3.8).
// https://lwn.net/Articles/533499/
#ifndef MAP_HUGE_SHIFT
  #define MAP_HUGE_SHIFT 26
#endif

// Define MADV_HUGEPAGE here so we can build HotSpot on old systems.
#ifndef MADV_HUGEPAGE
  #define MADV_HUGEPAGE 14
#endif

int os::Linux::commit_memory_impl(char* addr, size_t size,
                                  size_t alignment_hint, bool exec) {
  int err = os::Linux::commit_memory_impl(addr, size, exec);
  if (err == 0) {
    realign_memory(addr, size, alignment_hint);
  }
  return err;
}

bool os::pd_commit_memory(char* addr, size_t size, size_t alignment_hint,
                          bool exec) {
  return os::Linux::commit_memory_impl(addr, size, alignment_hint, exec) == 0;
}

void os::pd_commit_memory_or_exit(char* addr, size_t size,
                                  size_t alignment_hint, bool exec,
                                  const char* mesg) {
  assert(mesg != NULL, "mesg must be specified");
  int err = os::Linux::commit_memory_impl(addr, size, alignment_hint, exec);
  if (err != 0) {
    // the caller wants all commit errors to exit with the specified mesg:
    warn_fail_commit_memory(addr, size, alignment_hint, exec, err);
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "%s", mesg);
  }
}

void os::pd_realign_memory(char *addr, size_t bytes, size_t alignment_hint) {
  if (UseTransparentHugePages && alignment_hint > (size_t)vm_page_size()) {
    // We don't check the return value: madvise(MADV_HUGEPAGE) may not
    // be supported or the memory may already be backed by huge pages.
    ::madvise(addr, bytes, MADV_HUGEPAGE);
  }
}

void os::pd_free_memory(char *addr, size_t bytes, size_t alignment_hint) {
  // This method works by doing an mmap over an existing mmaping and effectively discarding
  // the existing pages. However it won't work for SHM-based large pages that cannot be
  // uncommitted at all. We don't do anything in this case to avoid creating a segment with
  // small pages on top of the SHM segment. This method always works for small pages, so we
  // allow that in any case.
  if (alignment_hint <= (size_t)os::vm_page_size() || can_commit_large_page_memory()) {
    commit_memory(addr, bytes, alignment_hint, !ExecMem);
  }
}

void os::numa_make_global(char *addr, size_t bytes) {
  Linux::numa_interleave_memory(addr, bytes);
}

// Define for numa_set_bind_policy(int). Setting the argument to 0 will set the
// bind policy to MPOL_PREFERRED for the current thread.
#define USE_MPOL_PREFERRED 0

void os::numa_make_local(char *addr, size_t bytes, int lgrp_hint) {
  // To make NUMA and large pages more robust when both enabled, we need to ease
  // the requirements on where the memory should be allocated. MPOL_BIND is the
  // default policy and it will force memory to be allocated on the specified
  // node. Changing this to MPOL_PREFERRED will prefer to allocate the memory on
  // the specified node, but will not force it. Using this policy will prevent
  // getting SIGBUS when trying to allocate large pages on NUMA nodes with no
  // free large pages.
  Linux::numa_set_bind_policy(USE_MPOL_PREFERRED);
  Linux::numa_tonode_memory(addr, bytes, lgrp_hint);
}

bool os::numa_topology_changed() { return false; }

size_t os::numa_get_groups_num() {
  // Return just the number of nodes in which it's possible to allocate memory
  // (in numa terminology, configured nodes).
  return Linux::numa_num_configured_nodes();
}

int os::numa_get_group_id() {
  int cpu_id = Linux::sched_getcpu();
  if (cpu_id != -1) {
    int lgrp_id = Linux::get_node_by_cpu(cpu_id);
    if (lgrp_id != -1) {
      return lgrp_id;
    }
  }
  return 0;
}

int os::numa_get_group_id_for_address(const void* address) {
  void** pages = const_cast<void**>(&address);
  int id = -1;

  if (os::Linux::numa_move_pages(0, 1, pages, NULL, &id, 0) == -1) {
    return -1;
  }
  if (id < 0) {
    return -1;
  }
  return id;
}

int os::Linux::get_existing_num_nodes() {
  int node;
  int highest_node_number = Linux::numa_max_node();
  int num_nodes = 0;

  // Get the total number of nodes in the system including nodes without memory.
  for (node = 0; node <= highest_node_number; node++) {
    if (is_node_in_existing_nodes(node)) {
      num_nodes++;
    }
  }
  return num_nodes;
}

size_t os::numa_get_leaf_groups(int *ids, size_t size) {
  int highest_node_number = Linux::numa_max_node();
  size_t i = 0;

  // Map all node ids in which it is possible to allocate memory. Also nodes are
  // not always consecutively available, i.e. available from 0 to the highest
  // node number. If the nodes have been bound explicitly using numactl membind,
  // then allocate memory from those nodes only.
  for (int node = 0; node <= highest_node_number; node++) {
    if (Linux::is_node_in_bound_nodes((unsigned int)node)) {
      ids[i++] = node;
    }
  }
  return i;
}

bool os::get_page_info(char *start, page_info* info) {
  return false;
}

char *os::scan_pages(char *start, char* end, page_info* page_expected,
                     page_info* page_found) {
  return end;
}


int os::Linux::sched_getcpu_syscall(void) {
  unsigned int cpu = 0;
  int retval = -1;

#if defined(IA32)
  #ifndef SYS_getcpu
    #define SYS_getcpu 318
  #endif
  retval = syscall(SYS_getcpu, &cpu, NULL, NULL);
#elif defined(AMD64)
// Unfortunately we have to bring all these macros here from vsyscall.h
// to be able to compile on old linuxes.
  #define __NR_vgetcpu 2
  #define VSYSCALL_START (-10UL << 20)
  #define VSYSCALL_SIZE 1024
  #define VSYSCALL_ADDR(vsyscall_nr) (VSYSCALL_START+VSYSCALL_SIZE*(vsyscall_nr))
  typedef long (*vgetcpu_t)(unsigned int *cpu, unsigned int *node, unsigned long *tcache);
  vgetcpu_t vgetcpu = (vgetcpu_t)VSYSCALL_ADDR(__NR_vgetcpu);
  retval = vgetcpu(&cpu, NULL, NULL);
#endif

  return (retval == -1) ? retval : cpu;
}

void os::Linux::sched_getcpu_init() {
  // sched_getcpu() should be in libc.
  set_sched_getcpu(CAST_TO_FN_PTR(sched_getcpu_func_t,
                                  dlsym(RTLD_DEFAULT, "sched_getcpu")));

  // If it's not, try a direct syscall.
  if (sched_getcpu() == -1) {
    set_sched_getcpu(CAST_TO_FN_PTR(sched_getcpu_func_t,
                                    (void*)&sched_getcpu_syscall));
  }

  if (sched_getcpu() == -1) {
    vm_exit_during_initialization("getcpu(2) system call not supported by kernel");
  }
}

// Something to do with the numa-aware allocator needs these symbols
extern "C" JNIEXPORT void numa_warn(int number, char *where, ...) { }
extern "C" JNIEXPORT void numa_error(char *where) { }

// Handle request to load libnuma symbol version 1.1 (API v1). If it fails
// load symbol from base version instead.
void* os::Linux::libnuma_dlsym(void* handle, const char *name) {
  void *f = dlvsym(handle, name, "libnuma_1.1");
  if (f == NULL) {
    f = dlsym(handle, name);
  }
  return f;
}

// Handle request to load libnuma symbol version 1.2 (API v2) only.
// Return NULL if the symbol is not defined in this particular version.
void* os::Linux::libnuma_v2_dlsym(void* handle, const char* name) {
  return dlvsym(handle, name, "libnuma_1.2");
}

// Check numa dependent syscalls
static bool numa_syscall_check() {
  // NUMA APIs depend on several syscalls. E.g., get_mempolicy is required for numa_get_membind and
  // numa_get_interleave_mask. But these dependent syscalls can be unsupported for various reasons.
  // Especially in dockers, get_mempolicy is not allowed with the default configuration. So it's necessary
  // to check whether the syscalls are available. Currently, only get_mempolicy is checked since checking
  // others like mbind would cause unexpected side effects.
#ifdef SYS_get_mempolicy
  int dummy = 0;
  if (syscall(SYS_get_mempolicy, &dummy, NULL, 0, (void*)&dummy, 3) == -1) {
    return false;
  }
#endif

  return true;
}

bool os::Linux::libnuma_init() {
  // Requires sched_getcpu() and numa dependent syscalls support
  if ((sched_getcpu() != -1) && numa_syscall_check()) {
    void *handle = dlopen("libnuma.so.1", RTLD_LAZY);
    if (handle != NULL) {
      set_numa_node_to_cpus(CAST_TO_FN_PTR(numa_node_to_cpus_func_t,
                                           libnuma_dlsym(handle, "numa_node_to_cpus")));
      set_numa_node_to_cpus_v2(CAST_TO_FN_PTR(numa_node_to_cpus_v2_func_t,
                                              libnuma_v2_dlsym(handle, "numa_node_to_cpus")));
      set_numa_max_node(CAST_TO_FN_PTR(numa_max_node_func_t,
                                       libnuma_dlsym(handle, "numa_max_node")));
      set_numa_num_configured_nodes(CAST_TO_FN_PTR(numa_num_configured_nodes_func_t,
                                                   libnuma_dlsym(handle, "numa_num_configured_nodes")));
      set_numa_available(CAST_TO_FN_PTR(numa_available_func_t,
                                        libnuma_dlsym(handle, "numa_available")));
      set_numa_tonode_memory(CAST_TO_FN_PTR(numa_tonode_memory_func_t,
                                            libnuma_dlsym(handle, "numa_tonode_memory")));
      set_numa_interleave_memory(CAST_TO_FN_PTR(numa_interleave_memory_func_t,
                                                libnuma_dlsym(handle, "numa_interleave_memory")));
      set_numa_interleave_memory_v2(CAST_TO_FN_PTR(numa_interleave_memory_v2_func_t,
                                                libnuma_v2_dlsym(handle, "numa_interleave_memory")));
      set_numa_set_bind_policy(CAST_TO_FN_PTR(numa_set_bind_policy_func_t,
                                              libnuma_dlsym(handle, "numa_set_bind_policy")));
      set_numa_bitmask_isbitset(CAST_TO_FN_PTR(numa_bitmask_isbitset_func_t,
                                               libnuma_dlsym(handle, "numa_bitmask_isbitset")));
      set_numa_distance(CAST_TO_FN_PTR(numa_distance_func_t,
                                       libnuma_dlsym(handle, "numa_distance")));
      set_numa_get_membind(CAST_TO_FN_PTR(numa_get_membind_func_t,
                                          libnuma_v2_dlsym(handle, "numa_get_membind")));
      set_numa_get_interleave_mask(CAST_TO_FN_PTR(numa_get_interleave_mask_func_t,
                                                  libnuma_v2_dlsym(handle, "numa_get_interleave_mask")));
      set_numa_move_pages(CAST_TO_FN_PTR(numa_move_pages_func_t,
                                         libnuma_dlsym(handle, "numa_move_pages")));
      set_numa_set_preferred(CAST_TO_FN_PTR(numa_set_preferred_func_t,
                                            libnuma_dlsym(handle, "numa_set_preferred")));

      if (numa_available() != -1) {
        set_numa_all_nodes((unsigned long*)libnuma_dlsym(handle, "numa_all_nodes"));
        set_numa_all_nodes_ptr((struct bitmask **)libnuma_dlsym(handle, "numa_all_nodes_ptr"));
        set_numa_nodes_ptr((struct bitmask **)libnuma_dlsym(handle, "numa_nodes_ptr"));
        set_numa_interleave_bitmask(_numa_get_interleave_mask());
        set_numa_membind_bitmask(_numa_get_membind());
        // Create an index -> node mapping, since nodes are not always consecutive
        _nindex_to_node = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<int>(0, mtInternal);
        rebuild_nindex_to_node_map();
        // Create a cpu -> node mapping
        _cpu_to_node = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<int>(0, mtInternal);
        rebuild_cpu_to_node_map();
        return true;
      }
    }
  }
  return false;
}

size_t os::Linux::default_guard_size(os::ThreadType thr_type) {

  if (THPStackMitigation) {
    // If THPs are unconditionally enabled, the following scenario can lead to huge RSS
    // - parent thread spawns, in quick succession, multiple child threads
    // - child threads are slow to start
    // - thread stacks of future child threads are adjacent and get merged into one large VMA
    //   by the kernel, and subsequently transformed into huge pages by khugepaged
    // - child threads come up, place JVM guard pages, thus splinter the large VMA, splinter
    //   the huge pages into many (still paged-in) small pages.
    // The result of that sequence are thread stacks that are fully paged-in even though the
    // threads did not even start yet.
    // We prevent that by letting the glibc allocate a guard page, which causes a VMA with different
    // permission bits to separate two ajacent thread stacks and therefore prevent merging stacks
    // into one VMA.
    //
    // Yes, this means we have two guard sections - the glibc and the JVM one - per thread. But the
    // cost for that one extra protected page is dwarfed from a large win in performance and memory
    // that avoiding interference by khugepaged buys us.
    return os::vm_page_size();
  }

  // Creating guard page is very expensive. Java thread has HotSpot
  // guard pages, only enable glibc guard page for non-Java threads.
  // (Remember: compiler thread is a Java thread, too!)
  return ((thr_type == java_thread || thr_type == compiler_thread) ? 0 : page_size());
}

void os::Linux::rebuild_nindex_to_node_map() {
  int highest_node_number = Linux::numa_max_node();

  nindex_to_node()->clear();
  for (int node = 0; node <= highest_node_number; node++) {
    if (Linux::is_node_in_existing_nodes(node)) {
      nindex_to_node()->append(node);
    }
  }
}

// rebuild_cpu_to_node_map() constructs a table mapping cpud id to node id.
// The table is later used in get_node_by_cpu().
void os::Linux::rebuild_cpu_to_node_map() {
  const size_t NCPUS = 32768; // Since the buffer size computation is very obscure
                              // in libnuma (possible values are starting from 16,
                              // and continuing up with every other power of 2, but less
                              // than the maximum number of CPUs supported by kernel), and
                              // is a subject to change (in libnuma version 2 the requirements
                              // are more reasonable) we'll just hardcode the number they use
                              // in the library.
  const size_t BitsPerCLong = sizeof(long) * CHAR_BIT;

  size_t cpu_num = processor_count();
  size_t cpu_map_size = NCPUS / BitsPerCLong;
  size_t cpu_map_valid_size =
    MIN2((cpu_num + BitsPerCLong - 1) / BitsPerCLong, cpu_map_size);

  cpu_to_node()->clear();
  cpu_to_node()->at_grow(cpu_num - 1);

  size_t node_num = get_existing_num_nodes();

  int distance = 0;
  int closest_distance = INT_MAX;
  int closest_node = 0;
  unsigned long *cpu_map = NEW_C_HEAP_ARRAY(unsigned long, cpu_map_size, mtInternal);
  for (size_t i = 0; i < node_num; i++) {
    // Check if node is configured (not a memory-less node). If it is not, find
    // the closest configured node. Check also if node is bound, i.e. it's allowed
    // to allocate memory from the node. If it's not allowed, map cpus in that node
    // to the closest node from which memory allocation is allowed.
    if (!is_node_in_configured_nodes(nindex_to_node()->at(i)) ||
        !is_node_in_bound_nodes(nindex_to_node()->at(i))) {
      closest_distance = INT_MAX;
      // Check distance from all remaining nodes in the system. Ignore distance
      // from itself, from another non-configured node, and from another non-bound
      // node.
      for (size_t m = 0; m < node_num; m++) {
        if (m != i &&
            is_node_in_configured_nodes(nindex_to_node()->at(m)) &&
            is_node_in_bound_nodes(nindex_to_node()->at(m))) {
          distance = numa_distance(nindex_to_node()->at(i), nindex_to_node()->at(m));
          // If a closest node is found, update. There is always at least one
          // configured and bound node in the system so there is always at least
          // one node close.
          if (distance != 0 && distance < closest_distance) {
            closest_distance = distance;
            closest_node = nindex_to_node()->at(m);
          }
        }
      }
     } else {
       // Current node is already a configured node.
       closest_node = nindex_to_node()->at(i);
     }

    // Get cpus from the original node and map them to the closest node. If node
    // is a configured node (not a memory-less node), then original node and
    // closest node are the same.
    if (numa_node_to_cpus(nindex_to_node()->at(i), cpu_map, cpu_map_size * sizeof(unsigned long)) != -1) {
      for (size_t j = 0; j < cpu_map_valid_size; j++) {
        if (cpu_map[j] != 0) {
          for (size_t k = 0; k < BitsPerCLong; k++) {
            if (cpu_map[j] & (1UL << k)) {
              int cpu_index = j * BitsPerCLong + k;

#ifndef PRODUCT
              if (UseDebuggerErgo1 && cpu_index >= (int)cpu_num) {
                // Some debuggers limit the processor count without
                // intercepting the NUMA APIs. Just fake the values.
                cpu_index = 0;
              }
#endif

              cpu_to_node()->at_put(cpu_index, closest_node);
            }
          }
        }
      }
    }
  }
  FREE_C_HEAP_ARRAY(unsigned long, cpu_map);
}

int os::Linux::numa_node_to_cpus(int node, unsigned long *buffer, int bufferlen) {
  // use the latest version of numa_node_to_cpus if available
  if (_numa_node_to_cpus_v2 != NULL) {

    // libnuma bitmask struct
    struct bitmask {
      unsigned long size; /* number of bits in the map */
      unsigned long *maskp;
    };

    struct bitmask mask;
    mask.maskp = (unsigned long *)buffer;
    mask.size = bufferlen * 8;
    return _numa_node_to_cpus_v2(node, &mask);
  } else if (_numa_node_to_cpus != NULL) {
    return _numa_node_to_cpus(node, buffer, bufferlen);
  }
  return -1;
}

int os::Linux::get_node_by_cpu(int cpu_id) {
  if (cpu_to_node() != NULL && cpu_id >= 0 && cpu_id < cpu_to_node()->length()) {
    return cpu_to_node()->at(cpu_id);
  }
  return -1;
}

GrowableArray<int>* os::Linux::_cpu_to_node;
GrowableArray<int>* os::Linux::_nindex_to_node;
os::Linux::sched_getcpu_func_t os::Linux::_sched_getcpu;
os::Linux::numa_node_to_cpus_func_t os::Linux::_numa_node_to_cpus;
os::Linux::numa_node_to_cpus_v2_func_t os::Linux::_numa_node_to_cpus_v2;
os::Linux::numa_max_node_func_t os::Linux::_numa_max_node;
os::Linux::numa_num_configured_nodes_func_t os::Linux::_numa_num_configured_nodes;
os::Linux::numa_available_func_t os::Linux::_numa_available;
os::Linux::numa_tonode_memory_func_t os::Linux::_numa_tonode_memory;
os::Linux::numa_interleave_memory_func_t os::Linux::_numa_interleave_memory;
os::Linux::numa_interleave_memory_v2_func_t os::Linux::_numa_interleave_memory_v2;
os::Linux::numa_set_bind_policy_func_t os::Linux::_numa_set_bind_policy;
os::Linux::numa_bitmask_isbitset_func_t os::Linux::_numa_bitmask_isbitset;
os::Linux::numa_distance_func_t os::Linux::_numa_distance;
os::Linux::numa_get_membind_func_t os::Linux::_numa_get_membind;
os::Linux::numa_get_interleave_mask_func_t os::Linux::_numa_get_interleave_mask;
os::Linux::numa_move_pages_func_t os::Linux::_numa_move_pages;
os::Linux::numa_set_preferred_func_t os::Linux::_numa_set_preferred;
os::Linux::NumaAllocationPolicy os::Linux::_current_numa_policy;
unsigned long* os::Linux::_numa_all_nodes;
struct bitmask* os::Linux::_numa_all_nodes_ptr;
struct bitmask* os::Linux::_numa_nodes_ptr;
struct bitmask* os::Linux::_numa_interleave_bitmask;
struct bitmask* os::Linux::_numa_membind_bitmask;

bool os::pd_uncommit_memory(char* addr, size_t size, bool exec) {
  uintptr_t res = (uintptr_t) ::mmap(addr, size, PROT_NONE,
                                     MAP_PRIVATE|MAP_FIXED|MAP_NORESERVE|MAP_ANONYMOUS, -1, 0);
  return res  != (uintptr_t) MAP_FAILED;
}

static address get_stack_commited_bottom(address bottom, size_t size) {
  address nbot = bottom;
  address ntop = bottom + size;

  size_t page_sz = os::vm_page_size();
  unsigned pages = size / page_sz;

  unsigned char vec[1];
  unsigned imin = 1, imax = pages + 1, imid;
  int mincore_return_value = 0;

  assert(imin <= imax, "Unexpected page size");

  while (imin < imax) {
    imid = (imax + imin) / 2;
    nbot = ntop - (imid * page_sz);

    // Use a trick with mincore to check whether the page is mapped or not.
    // mincore sets vec to 1 if page resides in memory and to 0 if page
    // is swapped output but if page we are asking for is unmapped
    // it returns -1,ENOMEM
    mincore_return_value = mincore(nbot, page_sz, vec);

    if (mincore_return_value == -1) {
      // Page is not mapped go up
      // to find first mapped page
      if (errno != EAGAIN) {
        assert(errno == ENOMEM, "Unexpected mincore errno");
        imax = imid;
      }
    } else {
      // Page is mapped go down
      // to find first not mapped page
      imin = imid + 1;
    }
  }

  nbot = nbot + page_sz;

  // Adjust stack bottom one page up if last checked page is not mapped
  if (mincore_return_value == -1) {
    nbot = nbot + page_sz;
  }

  return nbot;
}

bool os::committed_in_range(address start, size_t size, address& committed_start, size_t& committed_size) {
  int mincore_return_value;
  const size_t stripe = 1024;  // query this many pages each time
  unsigned char vec[stripe + 1];
  // set a guard
  vec[stripe] = 'X';

  const size_t page_sz = os::vm_page_size();
  size_t pages = size / page_sz;

  assert(is_aligned(start, page_sz), "Start address must be page aligned");
  assert(is_aligned(size, page_sz), "Size must be page aligned");

  committed_start = NULL;

  int loops = (pages + stripe - 1) / stripe;
  int committed_pages = 0;
  address loop_base = start;
  bool found_range = false;

  for (int index = 0; index < loops && !found_range; index ++) {
    assert(pages > 0, "Nothing to do");
    int pages_to_query = (pages >= stripe) ? stripe : pages;
    pages -= pages_to_query;

    // Get stable read
    while ((mincore_return_value = mincore(loop_base, pages_to_query * page_sz, vec)) == -1 && errno == EAGAIN);

    // During shutdown, some memory goes away without properly notifying NMT,
    // E.g. ConcurrentGCThread/WatcherThread can exit without deleting thread object.
    // Bailout and return as not committed for now.
    if (mincore_return_value == -1 && errno == ENOMEM) {
      return false;
    }

    assert(vec[stripe] == 'X', "overflow guard");
    assert(mincore_return_value == 0, "Range must be valid");
    // Process this stripe
    for (int vecIdx = 0; vecIdx < pages_to_query; vecIdx ++) {
      if ((vec[vecIdx] & 0x01) == 0) { // not committed
        // End of current contiguous region
        if (committed_start != NULL) {
          found_range = true;
          break;
        }
      } else { // committed
        // Start of region
        if (committed_start == NULL) {
          committed_start = loop_base + page_sz * vecIdx;
        }
        committed_pages ++;
      }
    }

    loop_base += pages_to_query * page_sz;
  }

  if (committed_start != NULL) {
    assert(committed_pages > 0, "Must have committed region");
    assert(committed_pages <= int(size / page_sz), "Can not commit more than it has");
    assert(committed_start >= start && committed_start < start + size, "Out of range");
    committed_size = page_sz * committed_pages;
    return true;
  } else {
    assert(committed_pages == 0, "Should not have committed region");
    return false;
  }
}


// Linux uses a growable mapping for the stack, and if the mapping for
// the stack guard pages is not removed when we detach a thread the
// stack cannot grow beyond the pages where the stack guard was
// mapped.  If at some point later in the process the stack expands to
// that point, the Linux kernel cannot expand the stack any further
// because the guard pages are in the way, and a segfault occurs.
//
// However, it's essential not to split the stack region by unmapping
// a region (leaving a hole) that's already part of the stack mapping,
// so if the stack mapping has already grown beyond the guard pages at
// the time we create them, we have to truncate the stack mapping.
// So, we need to know the extent of the stack mapping when
// create_stack_guard_pages() is called.

// We only need this for stacks that are growable: at the time of
// writing thread stacks don't use growable mappings (i.e. those
// creeated with MAP_GROWSDOWN), and aren't marked "[stack]", so this
// only applies to the main thread.

// If the (growable) stack mapping already extends beyond the point
// where we're going to put our guard pages, truncate the mapping at
// that point by munmap()ping it.  This ensures that when we later
// munmap() the guard pages we don't leave a hole in the stack
// mapping. This only affects the main/primordial thread

bool os::pd_create_stack_guard_pages(char* addr, size_t size) {
  if (os::is_primordial_thread()) {
    // As we manually grow stack up to bottom inside create_attached_thread(),
    // it's likely that os::Linux::initial_thread_stack_bottom is mapped and
    // we don't need to do anything special.
    // Check it first, before calling heavy function.
    uintptr_t stack_extent = (uintptr_t) os::Linux::initial_thread_stack_bottom();
    unsigned char vec[1];

    if (mincore((address)stack_extent, os::vm_page_size(), vec) == -1) {
      // Fallback to slow path on all errors, including EAGAIN
      assert((uintptr_t)addr >= stack_extent,
             "Sanity: addr should be larger than extent, " PTR_FORMAT " >= " PTR_FORMAT,
             p2i(addr), stack_extent);
      stack_extent = (uintptr_t) get_stack_commited_bottom(
                                                           os::Linux::initial_thread_stack_bottom(),
                                                           (size_t)addr - stack_extent);
    }

    if (stack_extent < (uintptr_t)addr) {
      ::munmap((void*)stack_extent, (uintptr_t)(addr - stack_extent));
    }
  }

  return os::commit_memory(addr, size, !ExecMem);
}

// If this is a growable mapping, remove the guard pages entirely by
// munmap()ping them.  If not, just call uncommit_memory(). This only
// affects the main/primordial thread, but guard against future OS changes.
// It's safe to always unmap guard pages for primordial thread because we
// always place it right after end of the mapped region.

bool os::remove_stack_guard_pages(char* addr, size_t size) {
  uintptr_t stack_extent, stack_base;

  if (os::is_primordial_thread()) {
    return ::munmap(addr, size) == 0;
  }

  return os::uncommit_memory(addr, size);
}

// 'requested_addr' is only treated as a hint, the return value may or
// may not start from the requested address. Unlike Linux mmap(), this
// function returns NULL to indicate failure.
static char* anon_mmap(char* requested_addr, size_t bytes) {
  // MAP_FIXED is intentionally left out, to leave existing mappings intact.
  const int flags = MAP_PRIVATE | MAP_NORESERVE | MAP_ANONYMOUS;

  // Map reserved/uncommitted pages PROT_NONE so we fail early if we
  // touch an uncommitted page. Otherwise, the read/write might
  // succeed if we have enough swap space to back the physical page.
  char* addr = (char*)::mmap(requested_addr, bytes, PROT_NONE, flags, -1, 0);

  return addr == MAP_FAILED ? NULL : addr;
}

// Allocate (using mmap, NO_RESERVE, with small pages) at either a given request address
//   (req_addr != NULL) or with a given alignment.
//  - bytes shall be a multiple of alignment.
//  - req_addr can be NULL. If not NULL, it must be a multiple of alignment.
//  - alignment sets the alignment at which memory shall be allocated.
//     It must be a multiple of allocation granularity.
// Returns address of memory or NULL. If req_addr was not NULL, will only return
//  req_addr or NULL.
static char* anon_mmap_aligned(char* req_addr, size_t bytes, size_t alignment) {
  size_t extra_size = bytes;
  if (req_addr == NULL && alignment > 0) {
    extra_size += alignment;
  }

  char* start = anon_mmap(req_addr, extra_size);
  if (start != NULL) {
    if (req_addr != NULL) {
      if (start != req_addr) {
        ::munmap(start, extra_size);
        start = NULL;
      }
    } else {
      char* const start_aligned = align_up(start, alignment);
      char* const end_aligned = start_aligned + bytes;
      char* const end = start + extra_size;
      if (start_aligned > start) {
        ::munmap(start, start_aligned - start);
      }
      if (end_aligned < end) {
        ::munmap(end_aligned, end - end_aligned);
      }
      start = start_aligned;
    }
  }
  return start;
}

static int anon_munmap(char * addr, size_t size) {
  return ::munmap(addr, size) == 0;
}

char* os::pd_reserve_memory(size_t bytes, bool exec) {
  return anon_mmap(NULL, bytes);
}

bool os::pd_release_memory(char* addr, size_t size) {
  return anon_munmap(addr, size);
}

#ifdef CAN_SHOW_REGISTERS_ON_ASSERT
extern char* g_assert_poison; // assertion poison page address
#endif

static bool linux_mprotect(char* addr, size_t size, int prot) {
  // Linux wants the mprotect address argument to be page aligned.
  char* bottom = (char*)align_down((intptr_t)addr, os::Linux::page_size());

  // According to SUSv3, mprotect() should only be used with mappings
  // established by mmap(), and mmap() always maps whole pages. Unaligned
  // 'addr' likely indicates problem in the VM (e.g. trying to change
  // protection of malloc'ed or statically allocated memory). Check the
  // caller if you hit this assert.
  assert(addr == bottom, "sanity check");

  size = align_up(pointer_delta(addr, bottom, 1) + size, os::Linux::page_size());
  // Don't log anything if we're executing in the poison page signal handling
  // context. It can lead to reentrant use of other parts of the VM code.
#ifdef CAN_SHOW_REGISTERS_ON_ASSERT
  if (addr != g_assert_poison)
#endif
  Events::log(NULL, "Protecting memory [" INTPTR_FORMAT "," INTPTR_FORMAT "] with protection modes %x", p2i(bottom), p2i(bottom+size), prot);
  return ::mprotect(bottom, size, prot) == 0;
}

// Set protections specified
bool os::protect_memory(char* addr, size_t bytes, ProtType prot,
                        bool is_committed) {
  unsigned int p = 0;
  switch (prot) {
  case MEM_PROT_NONE: p = PROT_NONE; break;
  case MEM_PROT_READ: p = PROT_READ; break;
  case MEM_PROT_RW:   p = PROT_READ|PROT_WRITE; break;
  case MEM_PROT_RWX:  p = PROT_READ|PROT_WRITE|PROT_EXEC; break;
  default:
    ShouldNotReachHere();
  }
  // is_committed is unused.
  return linux_mprotect(addr, bytes, p);
}

bool os::guard_memory(char* addr, size_t size) {
  return linux_mprotect(addr, size, PROT_NONE);
}

bool os::unguard_memory(char* addr, size_t size) {
  return linux_mprotect(addr, size, PROT_READ|PROT_WRITE);
}

bool os::Linux::transparent_huge_pages_sanity_check(bool warn,
                                                    size_t page_size) {
  bool result = false;
  void *p = mmap(NULL, page_size * 2, PROT_READ|PROT_WRITE,
                 MAP_ANONYMOUS|MAP_PRIVATE,
                 -1, 0);
  if (p != MAP_FAILED) {
    void *aligned_p = align_up(p, page_size);

    result = madvise(aligned_p, page_size, MADV_HUGEPAGE) == 0;

    munmap(p, page_size * 2);
  }

  if (warn && !result) {
    warning("TransparentHugePages is not supported by the operating system.");
  }

  return result;
}

int os::Linux::hugetlbfs_page_size_flag(size_t page_size) {
  if (page_size != HugePages::default_static_hugepage_size()) {
    return (exact_log2(page_size) << MAP_HUGE_SHIFT);
  }
  return 0;
}

bool os::Linux::hugetlbfs_sanity_check(bool warn, size_t page_size) {
  // Include the page size flag to ensure we sanity check the correct page size.
  int flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | hugetlbfs_page_size_flag(page_size);
  void *p = mmap(NULL, page_size, PROT_READ|PROT_WRITE, flags, -1, 0);

  if (p != MAP_FAILED) {
    // Mapping succeeded, sanity check passed.
    munmap(p, page_size);
    return true;
  } else {
      log_info(pagesize)("Large page size (" SIZE_FORMAT "%s) failed sanity check, "
                         "checking if smaller large page sizes are usable",
                         byte_size_in_exact_unit(page_size),
                         exact_unit_for_byte_size(page_size));
      for (size_t page_size_ = _page_sizes.next_smaller(page_size);
          page_size_ != (size_t)os::vm_page_size();
          page_size_ = _page_sizes.next_smaller(page_size_)) {
        flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | hugetlbfs_page_size_flag(page_size_);
        p = mmap(NULL, page_size_, PROT_READ|PROT_WRITE, flags, -1, 0);
        if (p != MAP_FAILED) {
          // Mapping succeeded, sanity check passed.
          munmap(p, page_size_);
          log_info(pagesize)("Large page size (" SIZE_FORMAT "%s) passed sanity check",
                             byte_size_in_exact_unit(page_size_),
                             exact_unit_for_byte_size(page_size_));
          return true;
        }
      }
  }

  if (warn) {
    warning("HugeTLBFS is not configured or not supported by the operating system.");
  }

  return false;
}

bool os::Linux::shm_hugetlbfs_sanity_check(bool warn, size_t page_size) {
  // Try to create a large shared memory segment.
  int shmid = shmget(IPC_PRIVATE, page_size, SHM_HUGETLB|IPC_CREAT|SHM_R|SHM_W);
  if (shmid == -1) {
    // Possible reasons for shmget failure:
    // 1. shmmax is too small for the request.
    //    > check shmmax value: cat /proc/sys/kernel/shmmax
    //    > increase shmmax value: echo "new_value" > /proc/sys/kernel/shmmax
    // 2. not enough large page memory.
    //    > check available large pages: cat /proc/meminfo
    //    > increase amount of large pages:
    //          sysctl -w vm.nr_hugepages=new_value
    //    > For more information regarding large pages please refer to:
    //      https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt
    if (warn) {
      warning("Large pages using UseSHM are not configured on this system.");
    }
    return false;
  }
  // Managed to create a segment, now delete it.
  shmctl(shmid, IPC_RMID, NULL);
  return true;
}

// From the coredump_filter documentation:
//
// - (bit 0) anonymous private memory
// - (bit 1) anonymous shared memory
// - (bit 2) file-backed private memory
// - (bit 3) file-backed shared memory
// - (bit 4) ELF header pages in file-backed private memory areas (it is
//           effective only if the bit 2 is cleared)
// - (bit 5) hugetlb private memory
// - (bit 6) hugetlb shared memory
// - (bit 7) dax private memory
// - (bit 8) dax shared memory
//
static void set_coredump_filter(CoredumpFilterBit bit) {
  FILE *f;
  long cdm;

  if ((f = fopen("/proc/self/coredump_filter", "r+")) == NULL) {
    return;
  }

  if (fscanf(f, "%lx", &cdm) != 1) {
    fclose(f);
    return;
  }

  long saved_cdm = cdm;
  rewind(f);
  cdm |= bit;

  if (cdm != saved_cdm) {
    fprintf(f, "%#lx", cdm);
  }

  fclose(f);
}

// Large page support

static size_t _large_page_size = 0;

void warn_no_large_pages_configured() {
  if (!FLAG_IS_DEFAULT(UseLargePages)) {
    log_warning(pagesize)("UseLargePages disabled, no large pages configured and available on the system.");
  }
}

bool os::Linux::setup_large_page_type(size_t page_size) {
  if (FLAG_IS_DEFAULT(UseHugeTLBFS) &&
      FLAG_IS_DEFAULT(UseSHM) &&
      FLAG_IS_DEFAULT(UseTransparentHugePages)) {

    // The type of large pages has not been specified by the user.

    // Try UseHugeTLBFS and then UseSHM.
    UseHugeTLBFS = UseSHM = true;

    // Don't try UseTransparentHugePages since there are known
    // performance issues with it turned on. This might change in the future.
    UseTransparentHugePages = false;
  }

  if (UseTransparentHugePages) {
    bool warn_on_failure = !FLAG_IS_DEFAULT(UseTransparentHugePages);
    if (transparent_huge_pages_sanity_check(warn_on_failure, page_size)) {
      UseHugeTLBFS = false;
      UseSHM = false;
      return true;
    }
    UseTransparentHugePages = false;
  }

  if (UseHugeTLBFS) {
    bool warn_on_failure = !FLAG_IS_DEFAULT(UseHugeTLBFS);
    if (hugetlbfs_sanity_check(warn_on_failure, page_size)) {
      UseSHM = false;
      return true;
    }
    UseHugeTLBFS = false;
  }

  if (UseSHM) {
    bool warn_on_failure = !FLAG_IS_DEFAULT(UseSHM);
    if (shm_hugetlbfs_sanity_check(warn_on_failure, page_size)) {
      return true;
    }
    UseSHM = false;
  }

  warn_no_large_pages_configured();
  return false;
}

struct LargePageInitializationLoggerMark {
  ~LargePageInitializationLoggerMark() {
    LogTarget(Info, pagesize) lt;
    if (lt.is_enabled()) {
      LogStream ls(lt);
      if (UseLargePages) {
        ls.print_cr("UseLargePages=1, UseTransparentHugePages=%d, UseHugeTLBFS=%d, UseSHM=%d",
                    UseTransparentHugePages, UseHugeTLBFS, UseSHM);
        ls.print("Large page support enabled. Usable page sizes: ");
        os::page_sizes().print_on(&ls);
        ls.print_cr(". Default large page size: " EXACTFMT ".", EXACTFMTARGS(os::large_page_size()));
      } else {
        ls.print("Large page support disabled.");
      }
    }
  }
};

void os::large_page_init() {
  LargePageInitializationLoggerMark logger;

  // Query OS information first.
  HugePages::initialize();

  // If THPs are unconditionally enabled (THP mode "always"), khugepaged may attempt to
  // coalesce small pages in thread stacks to huge pages. That costs a lot of memory and
  // is usually unwanted for thread stacks. Therefore we attempt to prevent THP formation in
  // thread stacks unless the user explicitly allowed THP formation by manually disabling
  // -XX:-THPStackMitigation.
  if (HugePages::thp_mode() == THPMode::always) {
    if (THPStackMitigation) {
      log_info(pagesize)("JVM will attempt to prevent THPs in thread stacks.");
    } else {
      log_info(pagesize)("JVM will *not* prevent THPs in thread stacks. This may cause high RSS.");
    }
  } else {
    FLAG_SET_ERGO(THPStackMitigation, false); // Mitigation not needed
  }

  // 1) Handle the case where we do not want to use huge pages
  if (!UseLargePages &&
      !UseTransparentHugePages &&
      !UseHugeTLBFS &&
      !UseSHM) {
    // Not using large pages.
    return;
  }

  if (!FLAG_IS_DEFAULT(UseLargePages) && !UseLargePages) {
    // The user explicitly turned off large pages.
    // Ignore the rest of the large pages flags.
    UseTransparentHugePages = false;
    UseHugeTLBFS = false;
    UseSHM = false;
    return;
  }

  // 2) check if the OS supports THPs resp. static hugepages.
  if (UseTransparentHugePages && !HugePages::supports_thp()) {
    if (!FLAG_IS_DEFAULT(UseTransparentHugePages)) {
      log_warning(pagesize)("UseTransparentHugePages disabled, transparent huge pages are not supported by the operating system.");
    }
    UseLargePages = UseTransparentHugePages = UseHugeTLBFS = UseSHM = false;
    return;
  }
  if (!UseTransparentHugePages && !HugePages::supports_static_hugepages()) {
    warn_no_large_pages_configured();
    UseLargePages = UseTransparentHugePages = UseHugeTLBFS = UseSHM = false;
    return;
  }

  if (UseTransparentHugePages) {
    // In THP mode:
    // - os::large_page_size() is the *THP page size*
    // - os::pagesizes() has two members, the THP page size and the system page size
    assert(HugePages::supports_thp() && HugePages::thp_pagesize() > 0, "Missing OS info");
    _large_page_size = HugePages::thp_pagesize();
    _page_sizes.add(_large_page_size);
    _page_sizes.add(os::vm_page_size());

  } else {

    // In static hugepage mode:
    // - os::large_page_size() is the default static hugepage size (/proc/meminfo "Hugepagesize")
    // - os::pagesizes() contains all hugepage sizes the kernel supports, regardless whether there
    //   are pages configured in the pool or not (from /sys/kernel/hugepages/hugepage-xxxx ...)
    os::PageSizes all_large_pages = HugePages::static_info().pagesizes();
    const size_t default_large_page_size = HugePages::default_static_hugepage_size();

    // 3) Consistency check and post-processing

    // Check LargePageSizeInBytes matches an available page size and if so set _large_page_size
    // using LargePageSizeInBytes as the maximum allowed large page size. If LargePageSizeInBytes
    // doesn't match an available page size set _large_page_size to default_large_page_size
    // and use it as the maximum.
   if (FLAG_IS_DEFAULT(LargePageSizeInBytes) ||
        LargePageSizeInBytes == 0 ||
        LargePageSizeInBytes == default_large_page_size) {
      _large_page_size = default_large_page_size;
      log_info(pagesize)("Using the default large page size: " SIZE_FORMAT "%s",
                         byte_size_in_exact_unit(_large_page_size),
                         exact_unit_for_byte_size(_large_page_size));
    } else {
      if (all_large_pages.contains(LargePageSizeInBytes)) {
        _large_page_size = LargePageSizeInBytes;
        log_info(pagesize)("Overriding default large page size (" SIZE_FORMAT "%s) "
                           "using LargePageSizeInBytes: " SIZE_FORMAT "%s",
                           byte_size_in_exact_unit(default_large_page_size),
                           exact_unit_for_byte_size(default_large_page_size),
                           byte_size_in_exact_unit(_large_page_size),
                           exact_unit_for_byte_size(_large_page_size));
      } else {
        _large_page_size = default_large_page_size;
        log_info(pagesize)("LargePageSizeInBytes is not a valid large page size (" SIZE_FORMAT "%s) "
                           "using the default large page size: " SIZE_FORMAT "%s",
                           byte_size_in_exact_unit(LargePageSizeInBytes),
                           exact_unit_for_byte_size(LargePageSizeInBytes),
                           byte_size_in_exact_unit(_large_page_size),
                           exact_unit_for_byte_size(_large_page_size));
      }
    }

    // Populate _page_sizes with large page sizes less than or equal to
    // _large_page_size.
    for (size_t page_size = _large_page_size; page_size != 0;
           page_size = all_large_pages.next_smaller(page_size)) {
      _page_sizes.add(page_size);
    }
  }

  // Now determine the type of large pages to use:
  UseLargePages = os::Linux::setup_large_page_type(_large_page_size);

  set_coredump_filter(LARGEPAGES_BIT);
}

#ifndef SHM_HUGETLB
  #define SHM_HUGETLB 04000
#endif

#define shm_warning_format(format, ...)              \
  do {                                               \
    if (UseLargePages &&                             \
        (!FLAG_IS_DEFAULT(UseLargePages) ||          \
         !FLAG_IS_DEFAULT(UseSHM) ||                 \
         !FLAG_IS_DEFAULT(LargePageSizeInBytes))) {  \
      warning(format, __VA_ARGS__);                  \
    }                                                \
  } while (0)

#define shm_warning(str) shm_warning_format("%s", str)

#define shm_warning_with_errno(str)                \
  do {                                             \
    int err = errno;                               \
    shm_warning_format(str " (error = %d)", err);  \
  } while (0)

static char* shmat_with_alignment(int shmid, size_t bytes, size_t alignment) {
  assert(is_aligned(bytes, alignment), "Must be divisible by the alignment");

  if (!is_aligned(alignment, SHMLBA)) {
    assert(false, "Code below assumes that alignment is at least SHMLBA aligned");
    return NULL;
  }

  // To ensure that we get 'alignment' aligned memory from shmat,
  // we pre-reserve aligned virtual memory and then attach to that.

  char* pre_reserved_addr = anon_mmap_aligned(NULL /* req_addr */, bytes, alignment);
  if (pre_reserved_addr == NULL) {
    // Couldn't pre-reserve aligned memory.
    shm_warning("Failed to pre-reserve aligned memory for shmat.");
    return NULL;
  }

  // SHM_REMAP is needed to allow shmat to map over an existing mapping.
  char* addr = (char*)shmat(shmid, pre_reserved_addr, SHM_REMAP);

  if ((intptr_t)addr == -1) {
    int err = errno;
    shm_warning_with_errno("Failed to attach shared memory.");

    assert(err != EACCES, "Unexpected error");
    assert(err != EIDRM,  "Unexpected error");
    assert(err != EINVAL, "Unexpected error");

    // Since we don't know if the kernel unmapped the pre-reserved memory area
    // we can't unmap it, since that would potentially unmap memory that was
    // mapped from other threads.
    return NULL;
  }

  return addr;
}

static char* shmat_at_address(int shmid, char* req_addr) {
  if (!is_aligned(req_addr, SHMLBA)) {
    assert(false, "Requested address needs to be SHMLBA aligned");
    return NULL;
  }

  char* addr = (char*)shmat(shmid, req_addr, 0);

  if ((intptr_t)addr == -1) {
    shm_warning_with_errno("Failed to attach shared memory.");
    return NULL;
  }

  return addr;
}

static char* shmat_large_pages(int shmid, size_t bytes, size_t alignment, char* req_addr) {
  // If a req_addr has been provided, we assume that the caller has already aligned the address.
  if (req_addr != NULL) {
    assert(is_aligned(req_addr, os::large_page_size()), "Must be divisible by the large page size");
    assert(is_aligned(req_addr, alignment), "Must be divisible by given alignment");
    return shmat_at_address(shmid, req_addr);
  }

  // Since shmid has been setup with SHM_HUGETLB, shmat will automatically
  // return large page size aligned memory addresses when req_addr == NULL.
  // However, if the alignment is larger than the large page size, we have
  // to manually ensure that the memory returned is 'alignment' aligned.
  if (alignment > os::large_page_size()) {
    assert(is_aligned(alignment, os::large_page_size()), "Must be divisible by the large page size");
    return shmat_with_alignment(shmid, bytes, alignment);
  } else {
    return shmat_at_address(shmid, NULL);
  }
}

char* os::Linux::reserve_memory_special_shm(size_t bytes, size_t alignment,
                                            char* req_addr, bool exec) {
  // "exec" is passed in but not used.  Creating the shared image for
  // the code cache doesn't have an SHM_X executable permission to check.
  assert(UseLargePages && UseSHM, "only for SHM large pages");
  assert(is_aligned(req_addr, os::large_page_size()), "Unaligned address");
  assert(is_aligned(req_addr, alignment), "Unaligned address");

  if (!is_aligned(bytes, os::large_page_size())) {
    return NULL; // Fallback to small pages.
  }

  // Create a large shared memory region to attach to based on size.
  // Currently, size is the total size of the heap.
  int shmid = shmget(IPC_PRIVATE, bytes, SHM_HUGETLB|IPC_CREAT|SHM_R|SHM_W);
  if (shmid == -1) {
    // Possible reasons for shmget failure:
    // 1. shmmax is too small for the request.
    //    > check shmmax value: cat /proc/sys/kernel/shmmax
    //    > increase shmmax value: echo "new_value" > /proc/sys/kernel/shmmax
    // 2. not enough large page memory.
    //    > check available large pages: cat /proc/meminfo
    //    > increase amount of large pages:
    //          sysctl -w vm.nr_hugepages=new_value
    //    > For more information regarding large pages please refer to:
    //      https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt
    //      Note 1: different Linux may use different name for this property,
    //            e.g. on Redhat AS-3 it is "hugetlb_pool".
    //      Note 2: it's possible there's enough physical memory available but
    //            they are so fragmented after a long run that they can't
    //            coalesce into large pages. Try to reserve large pages when
    //            the system is still "fresh".
    shm_warning_with_errno("Failed to reserve shared memory.");
    return NULL;
  }

  // Attach to the region.
  char* addr = shmat_large_pages(shmid, bytes, alignment, req_addr);

  // Remove shmid. If shmat() is successful, the actual shared memory segment
  // will be deleted when it's detached by shmdt() or when the process
  // terminates. If shmat() is not successful this will remove the shared
  // segment immediately.
  shmctl(shmid, IPC_RMID, NULL);

  return addr;
}

static void warn_on_commit_special_failure(char* req_addr, size_t bytes,
                                           size_t page_size, int error) {
  assert(error == ENOMEM, "Only expect to fail if no memory is available");

  bool warn_on_failure = UseLargePages &&
      (!FLAG_IS_DEFAULT(UseLargePages) ||
       !FLAG_IS_DEFAULT(UseHugeTLBFS) ||
       !FLAG_IS_DEFAULT(LargePageSizeInBytes));

  if (warn_on_failure) {
    char msg[128];
    jio_snprintf(msg, sizeof(msg), "Failed to reserve and commit memory. req_addr: "
                                   PTR_FORMAT " bytes: " SIZE_FORMAT " page size: "
                                   SIZE_FORMAT " (errno = %d).",
                                   req_addr, bytes, page_size, error);
    warning("%s", msg);
  }
}

bool os::Linux::commit_memory_special(size_t bytes,
                                      size_t page_size,
                                      char* req_addr,
                                      bool exec) {
  assert(UseLargePages && UseHugeTLBFS, "Should only get here when HugeTLBFS large pages are used");
  assert(is_aligned(bytes, page_size), "Unaligned size");
  assert(is_aligned(req_addr, page_size), "Unaligned address");
  assert(req_addr != NULL, "Must have a requested address for special mappings");

  int prot = exec ? PROT_READ|PROT_WRITE|PROT_EXEC : PROT_READ|PROT_WRITE;
  int flags = MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED;

  // For large pages additional flags are required.
  if (page_size > (size_t) os::vm_page_size()) {
    flags |= MAP_HUGETLB | hugetlbfs_page_size_flag(page_size);
  }
  char* addr = (char*)::mmap(req_addr, bytes, prot, flags, -1, 0);

  if (addr == MAP_FAILED) {
    warn_on_commit_special_failure(req_addr, bytes, page_size, errno);
    return false;
  }

  log_debug(pagesize)("Commit special mapping: " PTR_FORMAT ", size=" SIZE_FORMAT "%s, page size="
                      SIZE_FORMAT "%s",
                      p2i(addr), byte_size_in_exact_unit(bytes),
                      exact_unit_for_byte_size(bytes),
                      byte_size_in_exact_unit(page_size),
                      exact_unit_for_byte_size(page_size));
  assert(is_aligned(addr, page_size), "Must be");
  return true;
}

char* os::Linux::reserve_memory_special_huge_tlbfs(size_t bytes,
                                                   size_t alignment,
                                                   size_t page_size,
                                                   char* req_addr,
                                                   bool exec) {
  assert(UseLargePages && UseHugeTLBFS, "only for Huge TLBFS large pages");
  assert(is_aligned(req_addr, alignment), "Must be");
  assert(is_aligned(req_addr, page_size), "Must be");
  assert(is_aligned(alignment, os::vm_allocation_granularity()), "Must be");
  assert(_page_sizes.contains(page_size), "Must be a valid page size");
  assert(page_size > (size_t)os::vm_page_size(), "Must be a large page size");
  assert(bytes >= page_size, "Shouldn't allocate large pages for small sizes");

  // We only end up here when at least 1 large page can be used.
  // If the size is not a multiple of the large page size, we
  // will mix the type of pages used, but in a decending order.
  // Start off by reserving a range of the given size that is
  // properly aligned. At this point no pages are committed. If
  // a requested address is given it will be used and it must be
  // aligned to both the large page size and the given alignment.
  // The larger of the two will be used.
  size_t required_alignment = MAX(page_size, alignment);
  char* const aligned_start = anon_mmap_aligned(req_addr, bytes, required_alignment);
  if (aligned_start == NULL) {
    return NULL;
  }

  // First commit using large pages.
  size_t large_bytes = align_down(bytes, page_size);
  bool large_committed = commit_memory_special(large_bytes, page_size, aligned_start, exec);

  if (large_committed && bytes == large_bytes) {
    // The size was large page aligned so no additional work is
    // needed even if the commit failed.
    return aligned_start;
  }

  // The requested size requires some small pages as well.
  char* small_start = aligned_start + large_bytes;
  size_t small_size = bytes - large_bytes;
  if (!large_committed) {
    // Failed to commit large pages, so we need to unmap the
    // reminder of the orinal reservation.
    ::munmap(small_start, small_size);
    return NULL;
  }

  // Commit the remaining bytes using small pages.
  bool small_committed = commit_memory_special(small_size, os::vm_page_size(), small_start, exec);
  if (!small_committed) {
    // Failed to commit the remaining size, need to unmap
    // the large pages part of the reservation.
    ::munmap(aligned_start, large_bytes);
    return NULL;
  }
  return aligned_start;
}

char* os::pd_reserve_memory_special(size_t bytes, size_t alignment, size_t page_size,
                                    char* req_addr, bool exec) {
  assert(UseLargePages, "only for large pages");

  char* addr;
  if (UseSHM) {
    // No support for using specific page sizes with SHM.
    addr = os::Linux::reserve_memory_special_shm(bytes, alignment, req_addr, exec);
  } else {
    assert(UseHugeTLBFS, "must be");
    addr = os::Linux::reserve_memory_special_huge_tlbfs(bytes, alignment, page_size, req_addr, exec);
  }

  if (addr != NULL) {
    if (UseNUMAInterleaving) {
      numa_make_global(addr, bytes);
    }
  }

  return addr;
}

bool os::Linux::release_memory_special_shm(char* base, size_t bytes) {
  // detaching the SHM segment will also delete it, see reserve_memory_special_shm()
  return shmdt(base) == 0;
}

bool os::Linux::release_memory_special_huge_tlbfs(char* base, size_t bytes) {
  return pd_release_memory(base, bytes);
}

bool os::pd_release_memory_special(char* base, size_t bytes) {
  assert(UseLargePages, "only for large pages");
  bool res;

  if (UseSHM) {
    res = os::Linux::release_memory_special_shm(base, bytes);
  } else {
    assert(UseHugeTLBFS, "must be");
    res = os::Linux::release_memory_special_huge_tlbfs(base, bytes);
  }
  return res;
}

size_t os::large_page_size() {
  return _large_page_size;
}

// With SysV SHM the entire memory region must be allocated as shared
// memory.
// HugeTLBFS allows application to commit large page memory on demand.
// However, when committing memory with HugeTLBFS fails, the region
// that was supposed to be committed will lose the old reservation
// and allow other threads to steal that memory region. Because of this
// behavior we can't commit HugeTLBFS memory.
bool os::can_commit_large_page_memory() {
  return UseTransparentHugePages;
}

bool os::can_execute_large_page_memory() {
  return UseTransparentHugePages || UseHugeTLBFS;
}

char* os::pd_attempt_map_memory_to_file_at(char* requested_addr, size_t bytes, int file_desc) {
  assert(file_desc >= 0, "file_desc is not valid");
  char* result = pd_attempt_reserve_memory_at(requested_addr, bytes, !ExecMem);
  if (result != NULL) {
    if (replace_existing_mapping_with_file_mapping(result, bytes, file_desc) == NULL) {
      vm_exit_during_initialization(err_msg("Error in mapping Java heap at the given filesystem directory"));
    }
  }
  return result;
}

// Reserve memory at an arbitrary address, only if that area is
// available (and not reserved for something else).

char* os::pd_attempt_reserve_memory_at(char* requested_addr, size_t bytes, bool exec) {
  // Assert only that the size is a multiple of the page size, since
  // that's all that mmap requires, and since that's all we really know
  // about at this low abstraction level.  If we need higher alignment,
  // we can either pass an alignment to this method or verify alignment
  // in one of the methods further up the call chain.  See bug 5044738.
  assert(bytes % os::vm_page_size() == 0, "reserving unexpected size block");

  // Repeatedly allocate blocks until the block is allocated at the
  // right spot.

  // Linux mmap allows caller to pass an address as hint; give it a try first,
  // if kernel honors the hint then we can return immediately.
  char * addr = anon_mmap(requested_addr, bytes);
  if (addr == requested_addr) {
    return requested_addr;
  }

  if (addr != NULL) {
    // mmap() is successful but it fails to reserve at the requested address
    anon_munmap(addr, bytes);
  }

  return NULL;
}

// Used to convert frequent JVM_Yield() to nops
bool os::dont_yield() {
  return DontYieldALot;
}

// Linux CFS scheduler (since 2.6.23) does not guarantee sched_yield(2) will
// actually give up the CPU. Since skip buddy (v2.6.28):
//
// * Sets the yielding task as skip buddy for current CPU's run queue.
// * Picks next from run queue, if empty, picks a skip buddy (can be the yielding task).
// * Clears skip buddies for this run queue (yielding task no longer a skip buddy).
//
// An alternative is calling os::naked_short_nanosleep with a small number to avoid
// getting re-scheduled immediately.
//
void os::naked_yield() {
  sched_yield();
}

////////////////////////////////////////////////////////////////////////////////
// thread priority support

// Note: Normal Linux applications are run with SCHED_OTHER policy. SCHED_OTHER
// only supports dynamic priority, static priority must be zero. For real-time
// applications, Linux supports SCHED_RR which allows static priority (1-99).
// However, for large multi-threaded applications, SCHED_RR is not only slower
// than SCHED_OTHER, but also very unstable (my volano tests hang hard 4 out
// of 5 runs - Sep 2005).
//
// The following code actually changes the niceness of kernel-thread/LWP. It
// has an assumption that setpriority() only modifies one kernel-thread/LWP,
// not the entire user process, and user level threads are 1:1 mapped to kernel
// threads. It has always been the case, but could change in the future. For
// this reason, the code should not be used as default (ThreadPriorityPolicy=0).
// It is only used when ThreadPriorityPolicy=1 and may require system level permission
// (e.g., root privilege or CAP_SYS_NICE capability).

int os::java_to_os_priority[CriticalPriority + 1] = {
  19,              // 0 Entry should never be used

   4,              // 1 MinPriority
   3,              // 2
   2,              // 3

   1,              // 4
   0,              // 5 NormPriority
  -1,              // 6

  -2,              // 7
  -3,              // 8
  -4,              // 9 NearMaxPriority

  -5,              // 10 MaxPriority

  -5               // 11 CriticalPriority
};

static int prio_init() {
  if (ThreadPriorityPolicy == 1) {
    if (geteuid() != 0) {
      if (!FLAG_IS_DEFAULT(ThreadPriorityPolicy) && !FLAG_IS_JIMAGE_RESOURCE(ThreadPriorityPolicy)) {
        warning("-XX:ThreadPriorityPolicy=1 may require system level permission, " \
                "e.g., being the root user. If the necessary permission is not " \
                "possessed, changes to priority will be silently ignored.");
      }
    }
  }
  if (UseCriticalJavaThreadPriority) {
    os::java_to_os_priority[MaxPriority] = os::java_to_os_priority[CriticalPriority];
  }
  return 0;
}

OSReturn os::set_native_priority(Thread* thread, int newpri) {
  if (!UseThreadPriorities || ThreadPriorityPolicy == 0) return OS_OK;

  int ret = setpriority(PRIO_PROCESS, thread->osthread()->thread_id(), newpri);
  return (ret == 0) ? OS_OK : OS_ERR;
}

OSReturn os::get_native_priority(const Thread* const thread,
                                 int *priority_ptr) {
  if (!UseThreadPriorities || ThreadPriorityPolicy == 0) {
    *priority_ptr = java_to_os_priority[NormPriority];
    return OS_OK;
  }

  errno = 0;
  *priority_ptr = getpriority(PRIO_PROCESS, thread->osthread()->thread_id());
  return (*priority_ptr != -1 || errno == 0 ? OS_OK : OS_ERR);
}

// This is the fastest way to get thread cpu time on Linux.
// Returns cpu time (user+sys) for any thread, not only for current.
// POSIX compliant clocks are implemented in the kernels 2.6.16+.
// It might work on 2.6.10+ with a special kernel/glibc patch.
// For reference, please, see IEEE Std 1003.1-2004:
//   http://www.unix.org/single_unix_specification

jlong os::Linux::fast_thread_cpu_time(clockid_t clockid) {
  struct timespec tp;
  int status = clock_gettime(clockid, &tp);
  assert(status == 0, "clock_gettime error: %s", os::strerror(errno));
  return (tp.tv_sec * NANOSECS_PER_SEC) + tp.tv_nsec;
}

// Determine if the vmid is the parent pid for a child in a PID namespace.
// Return the namespace pid if so, otherwise -1.
int os::Linux::get_namespace_pid(int vmid) {
  char fname[24];
  int retpid = -1;

  snprintf(fname, sizeof(fname), "/proc/%d/status", vmid);
  FILE *fp = fopen(fname, "r");

  if (fp) {
    int pid, nspid;
    int ret;
    while (!feof(fp) && !ferror(fp)) {
      ret = fscanf(fp, "NSpid: %d %d", &pid, &nspid);
      if (ret == 1) {
        break;
      }
      if (ret == 2) {
        retpid = nspid;
        break;
      }
      for (;;) {
        int ch = fgetc(fp);
        if (ch == EOF || ch == (int)'\n') break;
      }
    }
    fclose(fp);
  }
  return retpid;
}

extern void report_error(char* file_name, int line_no, char* title,
                         char* format, ...);

// Some linux distributions (notably: Alpine Linux) include the
// grsecurity in the kernel. Of particular interest from a JVM perspective
// is PaX (https://pax.grsecurity.net/), which adds some security features
// related to page attributes. Specifically, the MPROTECT PaX functionality
// (https://pax.grsecurity.net/docs/mprotect.txt) prevents dynamic
// code generation by disallowing a (previously) writable page to be
// marked as executable. This is, of course, exactly what HotSpot does
// for both JIT compiled method, as well as for stubs, adapters, etc.
//
// Instead of crashing "lazily" when trying to make a page executable,
// this code probes for the presence of PaX and reports the failure
// eagerly.
static void check_pax(void) {
  // Zero doesn't generate code dynamically, so no need to perform the PaX check
#ifndef ZERO
  size_t size = os::Linux::page_size();

  void* p = ::mmap(NULL, size, PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    log_debug(os)("os_linux.cpp: check_pax: mmap failed (%s)" , os::strerror(errno));
    vm_exit_out_of_memory(size, OOM_MMAP_ERROR, "failed to allocate memory for PaX check.");
  }

  int res = ::mprotect(p, size, PROT_WRITE|PROT_EXEC);
  if (res == -1) {
    log_debug(os)("os_linux.cpp: check_pax: mprotect failed (%s)" , os::strerror(errno));
    vm_exit_during_initialization(
      "Failed to mark memory page as executable - check if grsecurity/PaX is enabled");
  }

  ::munmap(p, size);
#endif
}

// this is called _before_ most of the global arguments have been parsed
void os::init(void) {
  char dummy;   // used to get a guess on initial stack address

  clock_tics_per_sec = sysconf(_SC_CLK_TCK);

  Linux::set_page_size(sysconf(_SC_PAGESIZE));
  if (Linux::page_size() == -1) {
    fatal("os_linux.cpp: os::init: sysconf failed (%s)",
          os::strerror(errno));
  }
  _page_sizes.add(Linux::page_size());

  Linux::initialize_system_info();

#ifdef __GLIBC__
  Linux::_mallinfo = CAST_TO_FN_PTR(Linux::mallinfo_func_t, dlsym(RTLD_DEFAULT, "mallinfo"));
  Linux::_mallinfo2 = CAST_TO_FN_PTR(Linux::mallinfo2_func_t, dlsym(RTLD_DEFAULT, "mallinfo2"));
#endif // __GLIBC__

  os::Linux::CPUPerfTicks pticks;
  bool res = os::Linux::get_tick_information(&pticks, -1);

  if (res && pticks.has_steal_ticks) {
    has_initial_tick_info = true;
    initial_total_ticks = pticks.total;
    initial_steal_ticks = pticks.steal;
  }

  // _main_thread points to the thread that created/loaded the JVM.
  Linux::_main_thread = pthread_self();

  // retrieve entry point for pthread_setname_np
  Linux::_pthread_setname_np =
    (int(*)(pthread_t, const char*))dlsym(RTLD_DEFAULT, "pthread_setname_np");

  check_pax();

  os::Posix::init();

  initial_time_count = javaTimeNanos();
}

// To install functions for atexit system call
extern "C" {
  static void perfMemory_exit_helper() {
    perfMemory_exit();
  }
}

void os::pd_init_container_support() {
  OSContainer::init();
}

void os::Linux::numa_init() {

  // Java can be invoked as
  // 1. Without numactl and heap will be allocated/configured on all nodes as
  //    per the system policy.
  // 2. With numactl --interleave:
  //      Use numa_get_interleave_mask(v2) API to get nodes bitmask. The same
  //      API for membind case bitmask is reset.
  //      Interleave is only hint and Kernel can fallback to other nodes if
  //      no memory is available on the target nodes.
  // 3. With numactl --membind:
  //      Use numa_get_membind(v2) API to get nodes bitmask. The same API for
  //      interleave case returns bitmask of all nodes.
  // numa_all_nodes_ptr holds bitmask of all nodes.
  // numa_get_interleave_mask(v2) and numa_get_membind(v2) APIs returns correct
  // bitmask when externally configured to run on all or fewer nodes.

  if (!Linux::libnuma_init()) {
    FLAG_SET_ERGO(UseNUMA, false);
    FLAG_SET_ERGO(UseNUMAInterleaving, false); // Also depends on libnuma.
  } else {
    if ((Linux::numa_max_node() < 1) || Linux::is_bound_to_single_node()) {
      // If there's only one node (they start from 0) or if the process
      // is bound explicitly to a single node using membind, disable NUMA
      UseNUMA = false;
    } else {
      LogTarget(Info,os) log;
      LogStream ls(log);

      Linux::set_configured_numa_policy(Linux::identify_numa_policy());

      struct bitmask* bmp = Linux::_numa_membind_bitmask;
      const char* numa_mode = "membind";

      if (Linux::is_running_in_interleave_mode()) {
        bmp = Linux::_numa_interleave_bitmask;
        numa_mode = "interleave";
      }

      ls.print("UseNUMA is enabled and invoked in '%s' mode."
               " Heap will be configured using NUMA memory nodes:", numa_mode);

      for (int node = 0; node <= Linux::numa_max_node(); node++) {
        if (Linux::_numa_bitmask_isbitset(bmp, node)) {
          ls.print(" %d", node);
        }
      }
    }
  }

  // When NUMA requested, not-NUMA-aware allocations default to interleaving.
  if (UseNUMA && !UseNUMAInterleaving) {
    FLAG_SET_ERGO_IF_DEFAULT(UseNUMAInterleaving, true);
  }

  if (UseParallelGC && UseNUMA && UseLargePages && !can_commit_large_page_memory()) {
    // With SHM and HugeTLBFS large pages we cannot uncommit a page, so there's no way
    // we can make the adaptive lgrp chunk resizing work. If the user specified both
    // UseNUMA and UseLargePages (or UseSHM/UseHugeTLBFS) on the command line - warn
    // and disable adaptive resizing.
    if (UseAdaptiveSizePolicy || UseAdaptiveNUMAChunkSizing) {
      warning("UseNUMA is not fully compatible with SHM/HugeTLBFS large pages, "
              "disabling adaptive resizing (-XX:-UseAdaptiveSizePolicy -XX:-UseAdaptiveNUMAChunkSizing)");
      UseAdaptiveSizePolicy = false;
      UseAdaptiveNUMAChunkSizing = false;
    }
  }
}

// this is called _after_ the global arguments have been parsed
jint os::init_2(void) {

  // This could be set after os::Posix::init() but all platforms
  // have to set it the same so we have to mirror Solaris.
  DEBUG_ONLY(os::set_mutex_init_done();)

  os::Posix::init_2();

  Linux::fast_thread_clock_init();

  if (PosixSignals::init() == JNI_ERR) {
    return JNI_ERR;
  }

  // Check and sets minimum stack sizes against command line options
  if (Posix::set_minimum_stack_sizes() == JNI_ERR) {
    return JNI_ERR;
  }

#if defined(IA32) && !defined(ZERO)
  // Need to ensure we've determined the process's initial stack to
  // perform the workaround
  Linux::capture_initial_stack(JavaThread::stack_size_at_create());
  workaround_expand_exec_shield_cs_limit();
#else
  suppress_primordial_thread_resolution = Arguments::created_by_java_launcher();
  if (!suppress_primordial_thread_resolution) {
    Linux::capture_initial_stack(JavaThread::stack_size_at_create());
  }
#endif

  Linux::libpthread_init();
  Linux::sched_getcpu_init();
  log_info(os)("HotSpot is running with %s, %s",
               Linux::libc_version(), Linux::libpthread_version());

#ifdef __GLIBC__
  // Check if we need to adjust the stack size for glibc guard pages.
  init_adjust_stacksize_for_guard_pages();
#endif

  if (UseNUMA || UseNUMAInterleaving) {
    Linux::numa_init();
  }

  if (MaxFDLimit) {
    // set the number of file descriptors to max. print out error
    // if getrlimit/setrlimit fails but continue regardless.
    struct rlimit nbr_files;
    int status = getrlimit(RLIMIT_NOFILE, &nbr_files);
    if (status != 0) {
      log_info(os)("os::init_2 getrlimit failed: %s", os::strerror(errno));
    } else {
      nbr_files.rlim_cur = nbr_files.rlim_max;
      status = setrlimit(RLIMIT_NOFILE, &nbr_files);
      if (status != 0) {
        log_info(os)("os::init_2 setrlimit failed: %s", os::strerror(errno));
      }
    }
  }

  // at-exit methods are called in the reverse order of their registration.
  // atexit functions are called on return from main or as a result of a
  // call to exit(3C). There can be only 32 of these functions registered
  // and atexit() does not set errno.

  if (PerfAllowAtExitRegistration) {
    // only register atexit functions if PerfAllowAtExitRegistration is set.
    // atexit functions can be delayed until process exit time, which
    // can be problematic for embedded VM situations. Embedded VMs should
    // call DestroyJavaVM() to assure that VM resources are released.

    // note: perfMemory_exit_helper atexit function may be removed in
    // the future if the appropriate cleanup code can be added to the
    // VM_Exit VMOperation's doit method.
    if (atexit(perfMemory_exit_helper) != 0) {
      warning("os::init_2 atexit(perfMemory_exit_helper) failed");
    }
  }

  // initialize thread priority policy
  prio_init();

  if (!FLAG_IS_DEFAULT(AllocateHeapAt)) {
    set_coredump_filter(DAX_SHARED_BIT);
  }

  if (DumpPrivateMappingsInCore) {
    set_coredump_filter(FILE_BACKED_PVT_BIT);
  }

  if (DumpSharedMappingsInCore) {
    set_coredump_filter(FILE_BACKED_SHARED_BIT);
  }

  if (DumpPerfMapAtExit && FLAG_IS_DEFAULT(UseCodeCacheFlushing)) {
    // Disable code cache flushing to ensure the map file written at
    // exit contains all nmethods generated during execution.
    FLAG_SET_DEFAULT(UseCodeCacheFlushing, false);
  }

  // Override the timer slack value if needed. The adjustment for the main
  // thread will establish the setting for child threads, which would be
  // most threads in JDK/JVM.
  if (TimerSlack >= 0) {
    if (prctl(PR_SET_TIMERSLACK, TimerSlack) < 0) {
      vm_exit_during_initialization("Setting timer slack failed: %s", os::strerror(errno));
    }
  }

  return JNI_OK;
}

// older glibc versions don't have this macro (which expands to
// an optimized bit-counting function) so we have to roll our own
#ifndef CPU_COUNT

static int _cpu_count(const cpu_set_t* cpus) {
  int count = 0;
  // only look up to the number of configured processors
  for (int i = 0; i < os::processor_count(); i++) {
    if (CPU_ISSET(i, cpus)) {
      count++;
    }
  }
  return count;
}

#define CPU_COUNT(cpus) _cpu_count(cpus)

#endif // CPU_COUNT

// Get the current number of available processors for this process.
// This value can change at any time during a process's lifetime.
// sched_getaffinity gives an accurate answer as it accounts for cpusets.
// If it appears there may be more than 1024 processors then we do a
// dynamic check - see 6515172 for details.
// If anything goes wrong we fallback to returning the number of online
// processors - which can be greater than the number available to the process.
static int get_active_processor_count() {
  // Note: keep this function, with its CPU_xx macros, *outside* the os namespace (see JDK-8289477).
  cpu_set_t cpus;  // can represent at most 1024 (CPU_SETSIZE) processors
  cpu_set_t* cpus_p = &cpus;
  int cpus_size = sizeof(cpu_set_t);

  int configured_cpus = os::processor_count();  // upper bound on available cpus
  int cpu_count = 0;

// old build platforms may not support dynamic cpu sets
#ifdef CPU_ALLOC

  // To enable easy testing of the dynamic path on different platforms we
  // introduce a diagnostic flag: UseCpuAllocPath
  if (configured_cpus >= CPU_SETSIZE || UseCpuAllocPath) {
    // kernel may use a mask bigger than cpu_set_t
    log_trace(os)("active_processor_count: using dynamic path %s"
                  "- configured processors: %d",
                  UseCpuAllocPath ? "(forced) " : "",
                  configured_cpus);
    cpus_p = CPU_ALLOC(configured_cpus);
    if (cpus_p != NULL) {
      cpus_size = CPU_ALLOC_SIZE(configured_cpus);
      // zero it just to be safe
      CPU_ZERO_S(cpus_size, cpus_p);
    }
    else {
       // failed to allocate so fallback to online cpus
       int online_cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
       log_trace(os)("active_processor_count: "
                     "CPU_ALLOC failed (%s) - using "
                     "online processor count: %d",
                     os::strerror(errno), online_cpus);
       return online_cpus;
    }
  }
  else {
    log_trace(os)("active_processor_count: using static path - configured processors: %d",
                  configured_cpus);
  }
#else // CPU_ALLOC
// these stubs won't be executed
#define CPU_COUNT_S(size, cpus) -1
#define CPU_FREE(cpus)

  log_trace(os)("active_processor_count: only static path available - configured processors: %d",
                configured_cpus);
#endif // CPU_ALLOC

  // pid 0 means the current thread - which we have to assume represents the process
  if (sched_getaffinity(0, cpus_size, cpus_p) == 0) {
    if (cpus_p != &cpus) { // can only be true when CPU_ALLOC used
      cpu_count = CPU_COUNT_S(cpus_size, cpus_p);
    }
    else {
      cpu_count = CPU_COUNT(cpus_p);
    }
    log_trace(os)("active_processor_count: sched_getaffinity processor count: %d", cpu_count);
  }
  else {
    cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);
    warning("sched_getaffinity failed (%s)- using online processor count (%d) "
            "which may exceed available processors", os::strerror(errno), cpu_count);
  }

  if (cpus_p != &cpus) { // can only be true when CPU_ALLOC used
    CPU_FREE(cpus_p);
  }

  assert(cpu_count > 0 && cpu_count <= os::processor_count(), "sanity check");
  return cpu_count;
}

int os::Linux::active_processor_count() {
  return get_active_processor_count();
}

// Determine the active processor count from one of
// three different sources:
//
// 1. User option -XX:ActiveProcessorCount
// 2. kernel os calls (sched_getaffinity or sysconf(_SC_NPROCESSORS_ONLN)
// 3. extracted from cgroup cpu subsystem (shares and quotas)
//
// Option 1, if specified, will always override.
// If the cgroup subsystem is active and configured, we
// will return the min of the cgroup and option 2 results.
// This is required since tools, such as numactl, that
// alter cpu affinity do not update cgroup subsystem
// cpuset configuration files.
int os::active_processor_count() {
  // User has overridden the number of active processors
  if (ActiveProcessorCount > 0) {
    log_trace(os)("active_processor_count: "
                  "active processor count set by user : %d",
                  ActiveProcessorCount);
    return ActiveProcessorCount;
  }

  int active_cpus;
  if (OSContainer::is_containerized()) {
    active_cpus = OSContainer::active_processor_count();
    log_trace(os)("active_processor_count: determined by OSContainer: %d",
                   active_cpus);
  } else {
    active_cpus = os::Linux::active_processor_count();
  }

  return active_cpus;
}

static bool should_warn_invalid_processor_id() {
  if (os::processor_count() == 1) {
    // Don't warn if we only have one processor
    return false;
  }

  static volatile int warn_once = 1;

  if (Atomic::load(&warn_once) == 0 ||
      Atomic::xchg(&warn_once, 0) == 0) {
    // Don't warn more than once
    return false;
  }

  return true;
}

uint os::processor_id() {
  const int id = Linux::sched_getcpu();

  if (id < processor_count()) {
    return (uint)id;
  }

  // Some environments (e.g. openvz containers and the rr debugger) incorrectly
  // report a processor id that is higher than the number of processors available.
  // This is problematic, for example, when implementing CPU-local data structures,
  // where the processor id is used to index into an array of length processor_count().
  // If this happens we return 0 here. This is is safe since we always have at least
  // one processor, but it's not optimal for performance if we're actually executing
  // in an environment with more than one processor.
  if (should_warn_invalid_processor_id()) {
    log_warning(os)("Invalid processor id reported by the operating system "
                    "(got processor id %d, valid processor id range is 0-%d)",
                    id, processor_count() - 1);
    log_warning(os)("Falling back to assuming processor id is 0. "
                    "This could have a negative impact on performance.");
  }

  return 0;
}

void os::set_native_thread_name(const char *name) {
  if (Linux::_pthread_setname_np) {
    char buf [16]; // according to glibc manpage, 16 chars incl. '/0'
    snprintf(buf, sizeof(buf), "%s", name);
    buf[sizeof(buf) - 1] = '\0';
    const int rc = Linux::_pthread_setname_np(pthread_self(), buf);
    // ERANGE should not happen; all other errors should just be ignored.
    assert(rc != ERANGE, "pthread_setname_np failed");
  }
}

bool os::bind_to_processor(uint processor_id) {
  // Not yet implemented.
  return false;
}

////////////////////////////////////////////////////////////////////////////////
// debug support

bool os::find(address addr, outputStream* st) {
  Dl_info dlinfo;
  memset(&dlinfo, 0, sizeof(dlinfo));
  if (dladdr(addr, &dlinfo) != 0) {
    st->print(PTR_FORMAT ": ", p2i(addr));
    if (dlinfo.dli_sname != NULL && dlinfo.dli_saddr != NULL) {
      st->print("%s+" PTR_FORMAT, dlinfo.dli_sname,
                p2i(addr) - p2i(dlinfo.dli_saddr));
    } else if (dlinfo.dli_fbase != NULL) {
      st->print("<offset " PTR_FORMAT ">", p2i(addr) - p2i(dlinfo.dli_fbase));
    } else {
      st->print("<absolute address>");
    }
    if (dlinfo.dli_fname != NULL) {
      st->print(" in %s", dlinfo.dli_fname);
    }
    if (dlinfo.dli_fbase != NULL) {
      st->print(" at " PTR_FORMAT, p2i(dlinfo.dli_fbase));
    }
    st->cr();

    if (Verbose) {
      // decode some bytes around the PC
      address begin = clamp_address_in_page(addr-40, addr, os::vm_page_size());
      address end   = clamp_address_in_page(addr+40, addr, os::vm_page_size());
      address       lowest = (address) dlinfo.dli_sname;
      if (!lowest)  lowest = (address) dlinfo.dli_fbase;
      if (begin < lowest)  begin = lowest;
      Dl_info dlinfo2;
      if (dladdr(end, &dlinfo2) != 0 && dlinfo2.dli_saddr != dlinfo.dli_saddr
          && end > dlinfo2.dli_saddr && dlinfo2.dli_saddr > begin) {
        end = (address) dlinfo2.dli_saddr;
      }
      Disassembler::decode(begin, end, st);
    }
    return true;
  }
  return false;
}

////////////////////////////////////////////////////////////////////////////////
// misc

// This does not do anything on Linux. This is basically a hook for being
// able to use structured exception handling (thread-local exception filters)
// on, e.g., Win32.
void
os::os_exception_wrapper(java_call_t f, JavaValue* value, const methodHandle& method,
                         JavaCallArguments* args, JavaThread* thread) {
  f(value, method, args, thread);
}

// This code originates from JDK's sysOpen and open64_w
// from src/solaris/hpi/src/system_md.c

int os::open(const char *path, int oflag, int mode) {
  if (strlen(path) > MAX_PATH - 1) {
    errno = ENAMETOOLONG;
    return -1;
  }

  // All file descriptors that are opened in the Java process and not
  // specifically destined for a subprocess should have the close-on-exec
  // flag set.  If we don't set it, then careless 3rd party native code
  // might fork and exec without closing all appropriate file descriptors
  // (e.g. as we do in closeDescriptors in UNIXProcess.c), and this in
  // turn might:
  //
  // - cause end-of-file to fail to be detected on some file
  //   descriptors, resulting in mysterious hangs, or
  //
  // - might cause an fopen in the subprocess to fail on a system
  //   suffering from bug 1085341.
  //
  // (Yes, the default setting of the close-on-exec flag is a Unix
  // design flaw)
  //
  // See:
  // 1085341: 32-bit stdio routines should support file descriptors >255
  // 4843136: (process) pipe file descriptor from Runtime.exec not being closed
  // 6339493: (process) Runtime.exec does not close all file descriptors on Solaris 9
  //
  // Modern Linux kernels (after 2.6.23 2007) support O_CLOEXEC with open().
  // O_CLOEXEC is preferable to using FD_CLOEXEC on an open file descriptor
  // because it saves a system call and removes a small window where the flag
  // is unset.  On ancient Linux kernels the O_CLOEXEC flag will be ignored
  // and we fall back to using FD_CLOEXEC (see below).
#ifdef O_CLOEXEC
  oflag |= O_CLOEXEC;
#endif

  int fd = ::open64(path, oflag, mode);
  if (fd == -1) return -1;

  //If the open succeeded, the file might still be a directory
  {
    struct stat64 buf64;
    int ret = ::fstat64(fd, &buf64);
    int st_mode = buf64.st_mode;

    if (ret != -1) {
      if ((st_mode & S_IFMT) == S_IFDIR) {
        errno = EISDIR;
        ::close(fd);
        return -1;
      }
    } else {
      ::close(fd);
      return -1;
    }
  }

#ifdef FD_CLOEXEC
  // Validate that the use of the O_CLOEXEC flag on open above worked.
  // With recent kernels, we will perform this check exactly once.
  static sig_atomic_t O_CLOEXEC_is_known_to_work = 0;
  if (!O_CLOEXEC_is_known_to_work) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags != -1) {
      if ((flags & FD_CLOEXEC) != 0)
        O_CLOEXEC_is_known_to_work = 1;
      else
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
  }
#endif

  return fd;
}


// create binary file, rewriting existing file if required
int os::create_binary_file(const char* path, bool rewrite_existing) {
  int oflags = O_WRONLY | O_CREAT;
  oflags |= rewrite_existing ? O_TRUNC : O_EXCL;
  return ::open64(path, oflags, S_IREAD | S_IWRITE);
}

// return current position of file pointer
jlong os::current_file_offset(int fd) {
  return (jlong)::lseek64(fd, (off64_t)0, SEEK_CUR);
}

// move file pointer to the specified offset
jlong os::seek_to_file_offset(int fd, jlong offset) {
  return (jlong)::lseek64(fd, (off64_t)offset, SEEK_SET);
}

// This code originates from JDK's sysAvailable
// from src/solaris/hpi/src/native_threads/src/sys_api_td.c

int os::available(int fd, jlong *bytes) {
  jlong cur, end;
  int mode;
  struct stat64 buf64;

  if (::fstat64(fd, &buf64) >= 0) {
    mode = buf64.st_mode;
    if (S_ISCHR(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
      int n;
      if (::ioctl(fd, FIONREAD, &n) >= 0) {
        *bytes = n;
        return 1;
      }
    }
  }
  if ((cur = ::lseek64(fd, 0L, SEEK_CUR)) == -1) {
    return 0;
  } else if ((end = ::lseek64(fd, 0L, SEEK_END)) == -1) {
    return 0;
  } else if (::lseek64(fd, cur, SEEK_SET) == -1) {
    return 0;
  }
  *bytes = end - cur;
  return 1;
}

// Map a block of memory.
char* os::pd_map_memory(int fd, const char* file_name, size_t file_offset,
                        char *addr, size_t bytes, bool read_only,
                        bool allow_exec) {
  int prot;
  int flags = MAP_PRIVATE;

  if (read_only) {
    prot = PROT_READ;
  } else {
    prot = PROT_READ | PROT_WRITE;
  }

  if (allow_exec) {
    prot |= PROT_EXEC;
  }

  if (addr != NULL) {
    flags |= MAP_FIXED;
  }

  char* mapped_address = (char*)mmap(addr, (size_t)bytes, prot, flags,
                                     fd, file_offset);
  if (mapped_address == MAP_FAILED) {
    return NULL;
  }
  return mapped_address;
}


// Remap a block of memory.
char* os::pd_remap_memory(int fd, const char* file_name, size_t file_offset,
                          char *addr, size_t bytes, bool read_only,
                          bool allow_exec) {
  // same as map_memory() on this OS
  return os::map_memory(fd, file_name, file_offset, addr, bytes, read_only,
                        allow_exec);
}


// Unmap a block of memory.
bool os::pd_unmap_memory(char* addr, size_t bytes) {
  return munmap(addr, bytes) == 0;
}

static jlong slow_thread_cpu_time(Thread *thread, bool user_sys_cpu_time);

static jlong fast_cpu_time(Thread *thread) {
    clockid_t clockid;
    int rc = os::Linux::pthread_getcpuclockid(thread->osthread()->pthread_id(),
                                              &clockid);
    if (rc == 0) {
      return os::Linux::fast_thread_cpu_time(clockid);
    } else {
      // It's possible to encounter a terminated native thread that failed
      // to detach itself from the VM - which should result in ESRCH.
      assert_status(rc == ESRCH, rc, "pthread_getcpuclockid failed");
      return -1;
    }
}

// current_thread_cpu_time(bool) and thread_cpu_time(Thread*, bool)
// are used by JVM M&M and JVMTI to get user+sys or user CPU time
// of a thread.
//
// current_thread_cpu_time() and thread_cpu_time(Thread*) returns
// the fast estimate available on the platform.

jlong os::current_thread_cpu_time() {
  if (os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(CLOCK_THREAD_CPUTIME_ID);
  } else {
    // return user + sys since the cost is the same
    return slow_thread_cpu_time(Thread::current(), true /* user + sys */);
  }
}

jlong os::thread_cpu_time(Thread* thread) {
  // consistent with what current_thread_cpu_time() returns
  if (os::Linux::supports_fast_thread_cpu_time()) {
    return fast_cpu_time(thread);
  } else {
    return slow_thread_cpu_time(thread, true /* user + sys */);
  }
}

jlong os::current_thread_cpu_time(bool user_sys_cpu_time) {
  if (user_sys_cpu_time && os::Linux::supports_fast_thread_cpu_time()) {
    return os::Linux::fast_thread_cpu_time(CLOCK_THREAD_CPUTIME_ID);
  } else {
    return slow_thread_cpu_time(Thread::current(), user_sys_cpu_time);
  }
}

jlong os::thread_cpu_time(Thread *thread, bool user_sys_cpu_time) {
  if (user_sys_cpu_time && os::Linux::supports_fast_thread_cpu_time()) {
    return fast_cpu_time(thread);
  } else {
    return slow_thread_cpu_time(thread, user_sys_cpu_time);
  }
}

//  -1 on error.
static jlong slow_thread_cpu_time(Thread *thread, bool user_sys_cpu_time) {
  pid_t  tid = thread->osthread()->thread_id();
  char *s;
  char stat[2048];
  int statlen;
  char proc_name[64];
  int count;
  long sys_time, user_time;
  char cdummy;
  int idummy;
  long ldummy;
  FILE *fp;

  snprintf(proc_name, 64, "/proc/self/task/%d/stat", tid);
  fp = fopen(proc_name, "r");
  if (fp == NULL) return -1;
  statlen = fread(stat, 1, 2047, fp);
  stat[statlen] = '\0';
  fclose(fp);

  // Skip pid and the command string. Note that we could be dealing with
  // weird command names, e.g. user could decide to rename java launcher
  // to "java 1.4.2 :)", then the stat file would look like
  //                1234 (java 1.4.2 :)) R ... ...
  // We don't really need to know the command string, just find the last
  // occurrence of ")" and then start parsing from there. See bug 4726580.
  s = strrchr(stat, ')');
  if (s == NULL) return -1;

  // Skip blank chars
  do { s++; } while (s && isspace(*s));

  count = sscanf(s,"%c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu",
                 &cdummy, &idummy, &idummy, &idummy, &idummy, &idummy,
                 &ldummy, &ldummy, &ldummy, &ldummy, &ldummy,
                 &user_time, &sys_time);
  if (count != 13) return -1;
  if (user_sys_cpu_time) {
    return ((jlong)sys_time + (jlong)user_time) * (1000000000 / clock_tics_per_sec);
  } else {
    return (jlong)user_time * (1000000000 / clock_tics_per_sec);
  }
}

void os::current_thread_cpu_time_info(jvmtiTimerInfo *info_ptr) {
  info_ptr->max_value = ALL_64_BITS;       // will not wrap in less than 64 bits
  info_ptr->may_skip_backward = false;     // elapsed time not wall time
  info_ptr->may_skip_forward = false;      // elapsed time not wall time
  info_ptr->kind = JVMTI_TIMER_TOTAL_CPU;  // user+system time is returned
}

void os::thread_cpu_time_info(jvmtiTimerInfo *info_ptr) {
  info_ptr->max_value = ALL_64_BITS;       // will not wrap in less than 64 bits
  info_ptr->may_skip_backward = false;     // elapsed time not wall time
  info_ptr->may_skip_forward = false;      // elapsed time not wall time
  info_ptr->kind = JVMTI_TIMER_TOTAL_CPU;  // user+system time is returned
}

bool os::is_thread_cpu_time_supported() {
  return true;
}

// System loadavg support.  Returns -1 if load average cannot be obtained.
// Linux doesn't yet have a (official) notion of processor sets,
// so just return the system wide load average.
int os::loadavg(double loadavg[], int nelem) {
  return ::getloadavg(loadavg, nelem);
}

void os::pause() {
  char filename[MAX_PATH];
  if (PauseAtStartupFile && PauseAtStartupFile[0]) {
    jio_snprintf(filename, MAX_PATH, "%s", PauseAtStartupFile);
  } else {
    jio_snprintf(filename, MAX_PATH, "./vm.paused.%d", current_process_id());
  }

  int fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd != -1) {
    struct stat buf;
    ::close(fd);
    while (::stat(filename, &buf) == 0) {
      (void)::poll(NULL, 0, 100);
    }
  } else {
    jio_fprintf(stderr,
                "Could not open pause file '%s', continuing immediately.\n", filename);
  }
}

// Get the default path to the core file
// Returns the length of the string
int os::get_core_path(char* buffer, size_t bufferSize) {
  /*
   * Max length of /proc/sys/kernel/core_pattern is 128 characters.
   * See https://www.kernel.org/doc/Documentation/sysctl/kernel.txt
   */
  const int core_pattern_len = 129;
  char core_pattern[core_pattern_len] = {0};

  int core_pattern_file = ::open("/proc/sys/kernel/core_pattern", O_RDONLY);
  if (core_pattern_file == -1) {
    return -1;
  }

  ssize_t ret = ::read(core_pattern_file, core_pattern, core_pattern_len);
  ::close(core_pattern_file);
  if (ret <= 0 || ret >= core_pattern_len || core_pattern[0] == '\n') {
    return -1;
  }
  if (core_pattern[ret-1] == '\n') {
    core_pattern[ret-1] = '\0';
  } else {
    core_pattern[ret] = '\0';
  }

  // Replace the %p in the core pattern with the process id. NOTE: we do this
  // only if the pattern doesn't start with "|", and we support only one %p in
  // the pattern.
  char *pid_pos = strstr(core_pattern, "%p");
  const char* tail = (pid_pos != NULL) ? (pid_pos + 2) : "";  // skip over the "%p"
  int written;

  if (core_pattern[0] == '/') {
    if (pid_pos != NULL) {
      *pid_pos = '\0';
      written = jio_snprintf(buffer, bufferSize, "%s%d%s", core_pattern,
                             current_process_id(), tail);
    } else {
      written = jio_snprintf(buffer, bufferSize, "%s", core_pattern);
    }
  } else {
    char cwd[PATH_MAX];

    const char* p = get_current_directory(cwd, PATH_MAX);
    if (p == NULL) {
      return -1;
    }

    if (core_pattern[0] == '|') {
      written = jio_snprintf(buffer, bufferSize,
                             "\"%s\" (or dumping to %s/core.%d)",
                             &core_pattern[1], p, current_process_id());
    } else if (pid_pos != NULL) {
      *pid_pos = '\0';
      written = jio_snprintf(buffer, bufferSize, "%s/%s%d%s", p, core_pattern,
                             current_process_id(), tail);
    } else {
      written = jio_snprintf(buffer, bufferSize, "%s/%s", p, core_pattern);
    }
  }

  if (written < 0) {
    return -1;
  }

  if (((size_t)written < bufferSize) && (pid_pos == NULL) && (core_pattern[0] != '|')) {
    int core_uses_pid_file = ::open("/proc/sys/kernel/core_uses_pid", O_RDONLY);

    if (core_uses_pid_file != -1) {
      char core_uses_pid = 0;
      ssize_t ret = ::read(core_uses_pid_file, &core_uses_pid, 1);
      ::close(core_uses_pid_file);

      if (core_uses_pid == '1') {
        jio_snprintf(buffer + written, bufferSize - written,
                                          ".%d", current_process_id());
      }
    }
  }

  return strlen(buffer);
}

bool os::start_debugging(char *buf, int buflen) {
  int len = (int)strlen(buf);
  char *p = &buf[len];

  jio_snprintf(p, buflen-len,
               "\n\n"
               "Do you want to debug the problem?\n\n"
               "To debug, run 'gdb /proc/%d/exe %d'; then switch to thread " UINTX_FORMAT " (" INTPTR_FORMAT ")\n"
               "Enter 'yes' to launch gdb automatically (PATH must include gdb)\n"
               "Otherwise, press RETURN to abort...",
               os::current_process_id(), os::current_process_id(),
               os::current_thread_id(), os::current_thread_id());

  bool yes = os::message_box("Unexpected Error", buf);

  if (yes) {
    // yes, user asked VM to launch debugger
    jio_snprintf(buf, sizeof(char)*buflen, "gdb /proc/%d/exe %d",
                 os::current_process_id(), os::current_process_id());

    os::fork_and_exec(buf);
    yes = false;
  }
  return yes;
}


// Java/Compiler thread:
//
//   Low memory addresses
// P0 +------------------------+
//    |                        |\  Java thread created by VM does not have glibc
//    |    glibc guard page    | - guard page, attached Java thread usually has
//    |                        |/  1 glibc guard page.
// P1 +------------------------+ Thread::stack_base() - Thread::stack_size()
//    |                        |\
//    |  HotSpot Guard Pages   | - red, yellow and reserved pages
//    |                        |/
//    +------------------------+ StackOverflow::stack_reserved_zone_base()
//    |                        |\
//    |      Normal Stack      | -
//    |                        |/
// P2 +------------------------+ Thread::stack_base()
//
// Non-Java thread:
//
//   Low memory addresses
// P0 +------------------------+
//    |                        |\
//    |  glibc guard page      | - usually 1 page
//    |                        |/
// P1 +------------------------+ Thread::stack_base() - Thread::stack_size()
//    |                        |\
//    |      Normal Stack      | -
//    |                        |/
// P2 +------------------------+ Thread::stack_base()
//
// ** P1 (aka bottom) and size (P2 = P1 - size) are the address and stack size
//    returned from pthread_attr_getstack().
// ** If adjustStackSizeForGuardPages() is true the guard pages have been taken
//    out of the stack size given in pthread_attr. We work around this for
//    threads created by the VM. We adjust bottom to be P1 and size accordingly.
//
#ifndef ZERO
static void current_stack_region(address * bottom, size_t * size) {
  if (os::is_primordial_thread()) {
    // primordial thread needs special handling because pthread_getattr_np()
    // may return bogus value.
    *bottom = os::Linux::initial_thread_stack_bottom();
    *size   = os::Linux::initial_thread_stack_size();
  } else {
    pthread_attr_t attr;

    int rslt = pthread_getattr_np(pthread_self(), &attr);

    // JVM needs to know exact stack location, abort if it fails
    if (rslt != 0) {
      if (rslt == ENOMEM) {
        vm_exit_out_of_memory(0, OOM_MMAP_ERROR, "pthread_getattr_np");
      } else {
        fatal("pthread_getattr_np failed with error = %d", rslt);
      }
    }

    if (pthread_attr_getstack(&attr, (void **)bottom, size) != 0) {
      fatal("Cannot locate current stack attributes!");
    }

    if (os::Linux::adjustStackSizeForGuardPages()) {
      size_t guard_size = 0;
      rslt = pthread_attr_getguardsize(&attr, &guard_size);
      if (rslt != 0) {
        fatal("pthread_attr_getguardsize failed with error = %d", rslt);
      }
      *bottom += guard_size;
      *size   -= guard_size;
    }

    pthread_attr_destroy(&attr);

  }
  assert(os::current_stack_pointer() >= *bottom &&
         os::current_stack_pointer() < *bottom + *size, "just checking");
}

address os::current_stack_base() {
  address bottom;
  size_t size;
  current_stack_region(&bottom, &size);
  return (bottom + size);
}

size_t os::current_stack_size() {
  // This stack size includes the usable stack and HotSpot guard pages
  // (for the threads that have Hotspot guard pages).
  address bottom;
  size_t size;
  current_stack_region(&bottom, &size);
  return size;
}
#endif

static inline struct timespec get_mtime(const char* filename) {
  struct stat st;
  int ret = os::stat(filename, &st);
  assert(ret == 0, "failed to stat() file '%s': %s", filename, os::strerror(errno));
  return st.st_mtim;
}

int os::compare_file_modified_times(const char* file1, const char* file2) {
  struct timespec filetime1 = get_mtime(file1);
  struct timespec filetime2 = get_mtime(file2);
  int diff = filetime1.tv_sec - filetime2.tv_sec;
  if (diff == 0) {
    return filetime1.tv_nsec - filetime2.tv_nsec;
  }
  return diff;
}

bool os::supports_map_sync() {
  return true;
}

// CRaC

jlong os::Linux::restore_start_time() {
  if (!_restore_start_time) {
    return -1;
  }
  return _restore_start_time;
}

jlong os::Linux::uptime_since_restore() {
  if (!_restore_start_counter) {
    return -1;
  }
  return javaTimeNanos() - _restore_start_counter;
}

void VM_Crac::trace_cr(const char* msg, ...) {
  if (CRTrace) {
    va_list ap;
    va_start(ap, msg);
    _ostream->print("CR: ");
    _ostream->vprint_cr(msg, ap);
    va_end(ap);
  }
}

void VM_Crac::print_resources(const char* msg, ...) {
  if (CRPrintResourcesOnCheckpoint) {
    va_list ap;
    va_start(ap, msg);
    _ostream->vprint(msg, ap);
    va_end(ap);
  }
}

void os::Linux::vm_create_start() {
  if (!CRaCCheckpointTo) {
    return;
  }
  _vm_inited_fds.initialize();
}

/* taken from criu, that took this from kernel */
#define NFS_PREF ".nfs"
#define NFS_PREF_LEN ((unsigned)sizeof(NFS_PREF) - 1)
#define NFS_FILEID_LEN ((unsigned)sizeof(uint64_t) << 1)
#define NFS_COUNTER_LEN ((unsigned)sizeof(unsigned int) << 1)
#define NFS_LEN (NFS_PREF_LEN + NFS_FILEID_LEN + NFS_COUNTER_LEN)
static bool nfs_silly_rename(char* path) {
  char *sep = strrchr(path, '/');
  char *base = sep ? sep + 1 : path;
  if (strncmp(base, NFS_PREF, NFS_PREF_LEN)) {
    return false;
  }
  for (unsigned i = NFS_PREF_LEN; i < NFS_LEN; ++i) {
    if (!isxdigit(base[i])) {
      return false;
    }
  }
  return true;
}

static int readfdlink(int fd, char *link, size_t len) {
  char fdpath[64];
  snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
  int ret = readlink(fdpath, link, len);
  if (ret == -1) {
    return ret;
  }
  link[(unsigned)ret < len ? ret : len - 1] = '\0';
  return ret;
}

static bool same_stat(struct stat* st1, struct stat* st2) {
  return st1->st_dev == st2->st_dev &&
         st1->st_ino == st2->st_ino;
}

bool FdsInfo::same_fd(int fd1, int fd2) {
  if (!same_stat(get_stat(fd1), get_stat(fd2))) {
    return false;
  }

  int flags1 = fcntl(fd1, F_GETFL);
  int flags2 = fcntl(fd2, F_GETFL);
  if (flags1 != flags2) {
    return false;
  }

  const int test_flag = O_NONBLOCK;
  const int new_flags1 = flags1 ^ test_flag;
  fcntl(fd1, F_SETFL, new_flags1);
  if (fcntl(fd1, F_GETFL) != new_flags1) {
    // flag write ignored or handled differently,
    // don't know what to do
    return false;
  }

  const int new_flags2 = fcntl(fd2, F_GETFL);
  const bool are_same = new_flags1 == new_flags2;

  fcntl(fd1, flags1);

  return are_same;
}

void FdsInfo::initialize() {
  assert(!inited(), "should be called only once");

  const int max_fd = sysconf(_SC_OPEN_MAX);
  _fdinfos = NEW_C_HEAP_ARRAY(fdinfo, max_fd, mtInternal);
  int last_fd = -1;

  for (int i = 0; i < max_fd; ++i) {
    fdinfo* info = _fdinfos + i;
    int r = fstat(i, &info->stat);
    if (r == -1) {
      info->state = CLOSED;
      continue;
    }
    info->state = ROOT; // can be changed to DUP_OF_0 + N below
    info->mark = 0;
    last_fd = i;
  }
  _len = last_fd + 1;
  _fdinfos = REALLOC_C_HEAP_ARRAY(fdinfo, _fdinfos, _len, mtInternal);

  for (int i = 0; i < _len; ++i) {
    for (int j = 0; j < i; ++j) {
      if (get_state(j) == ROOT && same_fd(i, j)) {
        _fdinfos[i].state = (state_t)(DUP_OF_0 + j);
        break;
      }
    }

    if (get_state(i) == ROOT) {
      char fdpath[PATH_MAX];
      int r = readfdlink(i, fdpath, sizeof(fdpath));
      guarantee(-1 != r, "can't stat fd");
      if (get_stat(i)->st_nlink == 0 ||
          strstr(fdpath, "(deleted)") ||
          nfs_silly_rename(fdpath)) {
        mark(i, FdsInfo::M_CANT_RESTORE);
      }
    }
  }
}

static void mark_classpath_entry(FdsInfo *fds, char* cp) {
  struct stat st;
  if (-1 == stat(cp, &st)) {
    return;
  }
  for (int i = 0; i < fds->len(); ++i) {
    if (same_stat(&st, fds->get_stat(i))) {
      fds->mark(i, FdsInfo::M_CLASSPATH);
    }
  }
}

static void do_classpaths(void (*fn)(FdsInfo*, char*), FdsInfo *fds, char* classpath) {
  assert(SafepointSynchronize::is_at_safepoint(),
      "can't do nasty things with sysclasspath");
  char *cp = classpath;
  char *n;
  while ((n = strchr(cp, ':'))) {
    *n = '\0';
    fn(fds, cp);
    *n = ':';
    cp = n + 1;
  }
  mark_classpath_entry(fds, cp);
}


static void mark_all_in(FdsInfo *fds, char* dirpath) {
  DIR *dir = os::opendir(dirpath);
  if (!dir) {
    return;
  }

  struct dirent* dent;
  while ((dent = os::readdir(dir))) {
    for (int i = 0; i < fds->len(); ++i) {
      if (fds->get_state(i) != FdsInfo::ROOT) {
        continue;
      }
      struct stat* fstat = fds->get_stat(i);
      if (dent->d_ino == fstat->st_ino) {
        fds->mark(i, FdsInfo::M_CLASSPATH);
      }
    }
  }

  os::closedir(dir);
}

static void mark_persistent(FdsInfo *fds) {
  if (!_persistent_resources) {
    return;
  }

  for (int i = 0; i < _persistent_resources->length(); ++i) {
    PersistentResourceDesc* pr = _persistent_resources->adr_at(i);
    int fd = pr->_fd;
    if (fds->len() <= fd) {
      break;
    }
    if (fds->get_state(fd) != FdsInfo::ROOT) {
      continue;
    }
    struct stat* st = fds->get_stat(fd);
    if (st->st_dev == pr->_st_dev && st->st_ino == pr->_st_ino) {
      fds->mark(fd, FdsInfo::M_PERSISTENT);
    }
  }

  delete _persistent_resources;
  _persistent_resources = NULL;
}

static int cr_util_path(char* path, int len) {
  os::jvm_path(path, len);
  char *after_elem = NULL;
  // path is ".../lib/server/libjvm.so"
  for (int i = 0; i < 2; ++i) {
    after_elem = strrchr(path, '/');
    *after_elem = '\0';
  }
  return after_elem-path;
}

static bool compute_crengine() {
  if (!CREngine) {
    return true;
  }

  if (CREngine[0] == '/') {
    _crengine = CREngine;
    return true;
  }

  char path[JVM_MAXPATHLEN];
  int pathlen = cr_util_path(path, sizeof(path));
  strcat(path + pathlen, "/");
  strcat(path + pathlen, CREngine);

  struct stat st;
  if (0 != stat(path, &st)) {
    warning("Could not find %s: %s", path, strerror(errno));
    return false;
  }

  _crengine = os::strdup(path);
  return true;
}

static int call_crengine() {
  if (!_crengine) {
    return -1;
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror("cannot fork for crengine");
    return -1;
  }
  if (pid == 0) {
    execl(_crengine, _crengine, "checkpoint", CRaCCheckpointTo, NULL);
    perror("execl");
    exit(1);
  }

  int status;
  int ret;
  do {
    ret = waitpid(pid, &status, 0);
  } while (ret == -1 && errno == EINTR);

  if (ret == -1 || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status) == 0 ? 0 : -1;
}

class CracSHM {
  char _path[128];
public:
  CracSHM(int id) {
    int shmpathlen = snprintf(_path, sizeof(_path), "/crac_%d", id);
    if (shmpathlen < 0 || sizeof(_path) <= (size_t)shmpathlen) {
      fprintf(stderr, "shmpath is too long: %d\n", shmpathlen);
    }
  }

  int open(int mode) {
    int shmfd = shm_open(_path, mode, 0600);
    if (-1 == shmfd) {
      perror("shm_open");
    }
    return shmfd;
  }

  void unlink() {
    shm_unlink(_path);
  }
};

static int checkpoint_restore(int *shmid) {

  int cres = call_crengine();
  if (cres < 0) {
    return JVM_CHECKPOINT_ERROR;
  }

  sigset_t waitmask;
  sigemptyset(&waitmask);
  sigaddset(&waitmask, RESTORE_SIGNAL);

  siginfo_t info;
  int sig;
  do {
    sig = sigwaitinfo(&waitmask, &info);
  } while (sig == -1 && errno == EINTR);
  assert(sig == RESTORE_SIGNAL, "got what requested");

  if (CRTraceStartupTime) {
    tty->print_cr("STARTUPTIME " JLONG_FORMAT " restore-native", os::javaTimeNanos());
  }

  if (info.si_code != SI_QUEUE || info.si_int < 0) {
    tty->print("JVM: invalid info for restore provided: %s", info.si_code == SI_QUEUE ? "queued" : "not queued");
    if (info.si_code == SI_QUEUE) {
      tty->print(" code %d", info.si_int);
    }
    tty->cr();
    return JVM_CHECKPOINT_ERROR;
  }

  if (0 < info.si_int) {
    *shmid = info.si_int;
  }

  return JVM_CHECKPOINT_OK;
}

static const char* stat2strtype(mode_t mode) {
  switch (mode & S_IFMT) {
  case S_IFSOCK: return "socket";
  case S_IFLNK:  return "symlink";
  case S_IFREG:  return "regular";
  case S_IFBLK:  return "block";
  case S_IFDIR:  return "directory";
  case S_IFCHR:  return "character";
  case S_IFIFO:  return "fifo";
  default:       break;
  }
  return "unknown";
}

static int stat2stfail(mode_t mode) {
  switch (mode & S_IFMT) {
  case S_IFSOCK:
    return JVM_CR_FAIL_SOCK;
  case S_IFLNK:
  case S_IFREG:
  case S_IFBLK:
  case S_IFDIR:
  case S_IFCHR:
    return JVM_CR_FAIL_FILE;
  case S_IFIFO:
    return JVM_CR_FAIL_PIPE;
  default:
    break;
  }
  return JVM_CR_FAIL;
}

static bool find_sock_details(int sockino, const char* base, bool v6, char* buf, size_t sz) {
  char filename[16];
  snprintf(filename, sizeof(filename), "/proc/net/%s", base);
  FILE* f = fopen(filename, "r");
  if (!f) {
    return false;
  }
  int r = fscanf(f, "%*[^\n]");
  if (r) {} // suppress warn unused gcc diagnostic

  char la[33], ra[33];
  int lp, rp;
  int ino;
  //   sl  local_address         remote_address        st   tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
  //    0: 0100007F:08AE         00000000:0000         0A   00000000:00000000 00:00000000 00000000  1000        0 2988639
  //  %4d: %08X%08X%08X%08X:%04X %08X%08X%08X%08X:%04X %02X %08X:%08X         %02X:%08lX  %08X       %5u      %8d %d
  bool eof;
  do {
    eof = EOF == fscanf(f, "%*d: %[^:]:%X %[^:]:%X %*X %*X:%*X %*X:%*X %*X %*d %*d %d%*[^\n]\n",
        la, &lp, ra, &rp, &ino);
  } while (ino != sockino && !eof);
  fclose(f);

  if (ino != sockino) {
    return false;
  }

  struct in6_addr a6l, a6r;
  struct in_addr a4l, a4r;
  if (v6) {
    for (int i = 0; i < 4; ++i) {
      sscanf(la + i * 8, "%8" PRIX32, a6l.s6_addr32 + i);
      sscanf(ra + i * 8, "%8" PRIX32, a6r.s6_addr32 + i);
    }
  } else {
    sscanf(la, "%" PRIX32, &a4l.s_addr);
    sscanf(ra, "%" PRIX32, &a4r.s_addr);
  }

  int const af = v6 ? AF_INET6 : AF_INET;
  void* const laddr = v6 ? (void*)&a6l : (void*)&a4l;
  void* const raddr = v6 ? (void*)&a6r : (void*)&a4r;
  char lstrb[48], rstrb[48];
  const char* const lstr = ::inet_ntop(af, laddr, lstrb, sizeof(lstrb)) ? lstrb : "NONE";
  const char* const rstr = ::inet_ntop(af, raddr, rstrb, sizeof(rstrb)) ? rstrb : "NONE";
  int msgsz = snprintf(buf, sz, "%s localAddr %s localPort %d remoteAddr %s remotePort %d",
        base, lstr, lp, rstr, rp);
  return msgsz < (int)sz;
}

static const char* sock_details(const char* details, char* buf, size_t sz) {
  int sockino;
  if (sscanf(details, "socket:[%d]", &sockino) <= 0) {
    return details;
  }

  const char* bases[] = { "tcp", "udp", "tcp6", "udp6", NULL };
  for (const char** b = bases; *b; ++b) {
    if (find_sock_details(sockino, *b, 2 <= b - bases, buf, sz)) {
      return buf;
    }
  }

  return details;
}

bool VM_Crac::read_shm(int shmid) {
  CracSHM shm(shmid);
  int shmfd = shm.open(O_RDONLY);
  shm.unlink();
  if (shmfd < 0) {
    return false;
  }
  bool ret = _restore_parameters.read_from(shmfd);
  close(shmfd);
  return ret;
}

// If checkpoint is called throught the API, jcmd operation and jcmd output doesn't exist.
bool VM_Crac::is_socket_from_jcmd(int sock) {
  if (_attach_op == NULL)
    return false;
  int sock_fd = _attach_op->socket();
  return sock == sock_fd;
}

void VM_Crac::report_ok_to_jcmd_if_any() {
  if (_attach_op == NULL)
    return;
  bufferedStream* buf = static_cast<bufferedStream*>(_ostream);
  _attach_op->effectively_complete_raw(JNI_OK, buf);
  // redirect any further output to console
  _ostream = tty;
}

void VM_Crac::doit() {

  FdsInfo fds;
  do_classpaths(mark_classpath_entry, &fds, Arguments::get_sysclasspath());
  do_classpaths(mark_classpath_entry, &fds, Arguments::get_appclasspath());
  do_classpaths(mark_all_in, &fds, Arguments::get_ext_dirs());
  mark_persistent(&fds);

  bool ok = !_dry_run;
  for (int i = 0; i < fds.len(); ++i) {
    if (fds.get_state(i) == FdsInfo::CLOSED) {
      continue;
    }

    char detailsbuf[128];
    int linkret = readfdlink(i, detailsbuf, sizeof(detailsbuf));
    const char* details = 0 < linkret ? detailsbuf : "";
    print_resources("JVM: FD fd=%d type=%s: details1=\"%s\" ",
        i, stat2strtype(fds.get_stat(i)->st_mode), details);

    if (_vm_inited_fds.get_state(i, FdsInfo::CLOSED) != FdsInfo::CLOSED) {
      print_resources("OK: inherited from process env\n");
      continue;
    }

    struct stat* st = fds.get_stat(i);
    if (S_ISCHR(st->st_mode)) {
      const int mjr = major(st->st_rdev);
      const int mnr = minor(st->st_rdev);
      if (mjr == 1 && (mnr == 8 || mnr == 9)) {
        print_resources("OK: always available, random or urandom\n");
        continue;
      }
    }

    if (fds.check(i, FdsInfo::M_CLASSPATH) && !fds.check(i, FdsInfo::M_CANT_RESTORE)) {
      print_resources("OK: in classpath\n");
      continue;
    }

    if (fds.check(i, FdsInfo::M_PERSISTENT)) {
      print_resources("OK: assured persistent\n");
      continue;
    }

    if (S_ISSOCK(st->st_mode)) {
      if (is_socket_from_jcmd(i)){
        print_resources("OK: jcmd socket\n");
        continue;
      }
      details = sock_details(details, detailsbuf, sizeof(detailsbuf));
      print_resources(" details2=\"%s\" ", details);
    }

    print_resources("BAD: opened by application\n");
    ok = false;

    char* msg = NEW_C_HEAP_ARRAY(char, strlen(details) + 1, mtInternal);
    strcpy(msg, details);
    _failures->append(CracFailDep(stat2stfail(st->st_mode & S_IFMT), msg));
  }

  if (!ok && CRHeapDumpOnCheckpointException) {
    HeapDumper::dump_heap();
  }

  if (!ok && CRDoThrowCheckpointException) {
    return;
  }

  if (!PerfMemoryLinux::checkpoint(CRaCCheckpointTo)) {
    return;
  }

  int shmid = 0;
  if (CRAllowToSkipCheckpoint) {
    trace_cr("Skip Checkpoint");
  } else {
    trace_cr("Checkpoint ...");
    report_ok_to_jcmd_if_any();
    int ret = checkpoint_restore(&shmid);
    if (ret == JVM_CHECKPOINT_ERROR) {
      PerfMemoryLinux::restore();
      return;
    }
  }

  if (shmid <= 0 || !VM_Crac::read_shm(shmid)) {
    _restore_start_time = os::javaTimeMillis();
    _restore_start_counter = os::javaTimeNanos();
  }
  PerfMemoryLinux::restore();

  _ok = true;
}

void os::Linux::register_persistent_fd(int fd, int st_dev, int st_ino) {
  if (!CRaCCheckpointTo) {
    return;
  }
  if (!_persistent_resources) {
    _persistent_resources = new (ResourceObj::C_HEAP, mtInternal)
      GrowableArray<PersistentResourceDesc>(0, mtInternal/*C_heap*/);
  }
  int dup = -1;
  int i = 0;
  while (i < _persistent_resources->length()) {
    int pfd = _persistent_resources->adr_at(i)->_fd;
    if (pfd == fd) {
      dup = i;
      break;
    } else if (fd < pfd) {
      break;
    }
    ++i;
  }

  if (0 <= dup) {
    _persistent_resources->at_put(dup, PersistentResourceDesc(fd, st_dev, st_ino));
  } else {
    _persistent_resources->insert_before(i, PersistentResourceDesc(fd, st_dev, st_ino));
  }
}

void os::Linux::deregister_persistent_fd(int fd, int st_dev, int st_ino) {
  if (!CRaCCheckpointTo) {
    return;
  }
  if (!_persistent_resources) {
    return;
  }
  int i = 0;
  while (i < _persistent_resources->length()) {
    PersistentResourceDesc* pr = _persistent_resources->adr_at(i);
    if (pr->_fd == fd && pr->_st_dev == (dev_t)st_dev && pr->_st_ino == (ino_t)st_ino) {
      break;
    }
  }
  if (i < _persistent_resources->length()) {
    _persistent_resources->remove_at(i);
  }
}

bool os::Linux::prepare_checkpoint() {
  struct stat st;

  if (0 == stat(CRaCCheckpointTo, &st)) {
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
      warning("%s: not a directory", CRaCCheckpointTo);
      return false;
    }
  } else {
    if (-1 == mkdir(CRaCCheckpointTo, 0700)) {
      warning("cannot create %s: %s", CRaCCheckpointTo, strerror(errno));
      return false;
    }
    if (-1 == rmdir(CRaCCheckpointTo)) {
      warning("cannot cleanup after check: %s", strerror(errno));
      // not fatal
    }
  }

  if (!compute_crengine()) {
    return false;
  }

  return true;
}

static Handle ret_cr(int ret, Handle new_args, Handle new_props, Handle err_codes, Handle err_msgs, TRAPS) {
  objArrayOop bundleObj = oopFactory::new_objectArray(5, CHECK_NH);
  objArrayHandle bundle(THREAD, bundleObj);
  jvalue jval = { .i = ret };
  oop retObj = java_lang_boxing_object::create(T_INT, &jval, CHECK_NH);
  bundle->obj_at_put(0, retObj);
  bundle->obj_at_put(1, new_args());
  bundle->obj_at_put(2, new_props());
  bundle->obj_at_put(3, err_codes());
  bundle->obj_at_put(4, err_msgs());
  return bundle;
}

/** Checkpoint main entry.
 */
Handle os::Linux::checkpoint(bool dry_run, jlong jcmd_stream, TRAPS) {
  if (!CRaCCheckpointTo) {
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  if (-1 == mkdir(CRaCCheckpointTo, 0700) && errno != EEXIST) {
    warning("cannot create %s: %s", CRaCCheckpointTo, strerror(errno));
    return ret_cr(JVM_CHECKPOINT_NONE, Handle(), Handle(), Handle(), Handle(), THREAD);
  }

  Universe::heap()->set_cleanup_unused(true);
  Universe::heap()->collect(GCCause::_full_gc_alot);
  Universe::heap()->set_cleanup_unused(false);

  VM_Crac cr(dry_run, (bufferedStream*)jcmd_stream);
  {
    MutexLocker ml(Heap_lock);
    VMThread::execute(&cr);
  }
  if (cr.ok()) {
    oop new_args = NULL;
    if (cr.new_args()) {
      new_args = java_lang_String::create_oop_from_str(cr.new_args(), CHECK_NH);
    }
    GrowableArray<const char *>* new_properties = cr.new_properties();
    objArrayOop propsObj = oopFactory::new_objArray(vmClasses::String_klass(), new_properties->length(), CHECK_NH);
    objArrayHandle props(THREAD, propsObj);

    for (int i = 0; i < new_properties->length(); i++) {
      oop propObj = java_lang_String::create_oop_from_str(new_properties->at(i), CHECK_NH);
      props->obj_at_put(i, propObj);
    }
    return ret_cr(JVM_CHECKPOINT_OK, Handle(THREAD, new_args), props, Handle(), Handle(), THREAD);
  }

  GrowableArray<CracFailDep>* failures = cr.failures();

  typeArrayOop codesObj = oopFactory::new_intArray(failures->length(), CHECK_NH);
  typeArrayHandle codes(THREAD, codesObj);
  objArrayOop msgsObj = oopFactory::new_objArray(vmClasses::String_klass(), failures->length(), CHECK_NH);
  objArrayHandle msgs(THREAD, msgsObj);

  for (int i = 0; i < failures->length(); ++i) {
    codes->int_at_put(i, failures->at(i)._type);
    oop msgObj = java_lang_String::create_oop_from_str(failures->at(i)._msg, CHECK_NH);
    FREE_C_HEAP_ARRAY(char, failures->at(i)._msg);
    msgs->obj_at_put(i, msgObj);
  }

  return ret_cr(JVM_CHECKPOINT_ERROR, Handle(), Handle(), codes, msgs, THREAD);
}

void os::Linux::restore() {
  struct stat st;

  jlong restore_time = javaTimeMillis();
  jlong restore_counter = javaTimeNanos();

  compute_crengine();

  int id = getpid();
  CracSHM shm(id);
  int shmfd = shm.open(O_RDWR | O_CREAT);
  if (0 <= shmfd) {
    if (CracRestoreParameters::write_to(
          shmfd,
          Arguments::system_properties(),
          Arguments::java_command() ? Arguments::java_command() : "",
          restore_time,
          restore_counter)) {
      char strid[32];
      snprintf(strid, sizeof(strid), "%d", id);
      setenv("CRAC_NEW_ARGS_ID", strid, true);
    }
    close(shmfd);
  }

  char strid[32];

  if (_crengine) {
    execl(_crengine, _crengine, "restore", CRaCRestoreFrom, NULL);
    warning("cannot execute \"%s restore ...\" (%s)", _crengine, strerror(errno));
  }

}

bool CracRestoreParameters::read_from(int fd) {
  struct stat st;
  if (fstat(fd, &st)) {
    perror("fstat (ignoring restore parameters)");
    return false;
  }

  char *contents = NEW_C_HEAP_ARRAY(char, st.st_size, mtInternal);
  if (read(fd, contents, st.st_size) < 0) {
    perror("read (ignoring restore parameters)");
    FREE_C_HEAP_ARRAY(char, contents);
    return false;
  }

  _raw_content = contents;

  // parse the contents to read new system properties and arguments
  header* hdr = (header*)_raw_content;
  char* cursor = _raw_content + sizeof(header);

  ::_restore_start_time = hdr->_restore_time;
  ::_restore_start_counter = hdr->_restore_counter;

  for (int i = 0; i < hdr->_nprops; i++) {
    assert((cursor + strlen(cursor) <= contents + st.st_size), "property length exceeds shared memory size");
    int idx = _properties->append(cursor);
    int prop_len = strlen(cursor) + 1;
    cursor = cursor + prop_len;
  }

  // left this pointer unowned, it is freed when process dies
  char* env_mem = NEW_C_HEAP_ARRAY(char, hdr->_env_memory_size, mtArguments);
  memcpy(env_mem, cursor, hdr->_env_memory_size);

  const char* env_end = env_mem + hdr->_env_memory_size;
  while (env_mem < env_end) {
    const size_t s = strlen(env_mem) + 1;
    assert(env_mem + s <= env_end, "env vars exceed memory buffer, maybe ending 0 is lost");
    putenv(env_mem);
    env_mem += s;
  }
  cursor += hdr->_env_memory_size;

  _args = cursor;
  return true;
}


void os::print_memory_mappings(char* addr, size_t bytes, outputStream* st) {
  // Note: all ranges are "[..)"
  unsigned long long start = (unsigned long long)addr;
  unsigned long long end = start + bytes;
  FILE* f = ::fopen("/proc/self/maps", "r");
  int num_found = 0;
  if (f != NULL) {
    st->print_cr("Range [%llx-%llx) contains: ", start, end);
    char line[512];
    while(fgets(line, sizeof(line), f) == line) {
      unsigned long long segment_start = 0;
      unsigned long long segment_end = 0;
      if (::sscanf(line, "%llx-%llx", &segment_start, &segment_end) == 2) {
        // Lets print out every range which touches ours.
        if (segment_start < end && segment_end > start) {
          num_found ++;
          st->print("%s", line); // line includes \n
        }
      }
    }
    ::fclose(f);
    if (num_found == 0) {
      st->print_cr("nothing.");
    }
    st->cr();
  }
}

bool os::trim_native_heap(os::size_change_t* rss_change) {
#ifdef __GLIBC__
  os::Linux::meminfo_t info1;
  os::Linux::meminfo_t info2;

  bool have_info1 = rss_change != nullptr &&
                    os::Linux::query_process_memory_info(&info1);
  ::malloc_trim(0);
  bool have_info2 = rss_change != nullptr && have_info1 &&
                    os::Linux::query_process_memory_info(&info2);
  ssize_t delta = (ssize_t) -1;
  if (rss_change != nullptr) {
    if (have_info1 && have_info2 &&
        info1.vmrss != -1 && info2.vmrss != -1 &&
        info1.vmswap != -1 && info2.vmswap != -1) {
      // Note: query_process_memory_info returns values in K
      rss_change->before = (info1.vmrss + info1.vmswap) * K;
      rss_change->after = (info2.vmrss + info2.vmswap) * K;
    } else {
      rss_change->after = rss_change->before = SIZE_MAX;
    }
  }

  return true;
#else
  return false; // musl
#endif
}
