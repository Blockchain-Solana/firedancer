#if !defined(__linux__)
# error "Target operating system is unsupported by seccomp."
#endif

#define _GNU_SOURCE

#include "fd_sandbox.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/securebits.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/capability.h>
#include <dirent.h>
#include <limits.h>
#include <sched.h>        /* CLONE_*, setns, unshare */
#include <stddef.h>
#include <stdlib.h>       /* clearenv, mkdtemp*/
#include <sys/mount.h>    /* MS_*, MNT_*, mount, umount2 */
#include <sys/prctl.h>
#include <sys/resource.h> /* RLIMIT_*, rlimit, setrlimit */
#include <sys/stat.h>     /* mkdir */
#include <sys/syscall.h>  /* SYS_* */
#include <unistd.h>       /* set*id, sysconf, close, chdir, rmdir syscall */

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define X32_SYSCALL_BIT 0x40000000

#if defined(__i386__)
# define ARCH_NR  AUDIT_ARCH_I386
#elif defined(__x86_64__)
# define ARCH_NR  AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
# define ARCH_NR AUDIT_ARCH_AARCH64
#else
# error "Target architecture is unsupported by seccomp."
#endif

#define FD_TESTV(c) do { if( FD_UNLIKELY( !(c) ) ) FD_LOG_ERR(( "FAIL: (%d:%s) %s", errno, strerror( errno ), #c )); } while(0)

static void
secure_clear_environment( void ) {
  char** env = environ;
  while ( *env ) {
    size_t len = strlen( *env );
    explicit_bzero( *env, len );
    env++;
  }
  clearenv();
}

static void
setup_mountns( void ) {
  FD_TESTV( unshare ( CLONE_NEWNS ) == 0 );

  char temp_dir [] = "/tmp/fd-sandbox-XXXXXX";
  FD_TESTV( mkdtemp( temp_dir ) );
  FD_TESTV( !mount( NULL, "/", NULL, MS_SLAVE | MS_REC, NULL ) );
  FD_TESTV( !mount( temp_dir, temp_dir, NULL, MS_BIND | MS_REC, NULL ) );
  FD_TESTV( !chdir( temp_dir ) );
  FD_TESTV( !mkdir( "old-root", S_IRUSR | S_IWUSR ));
  FD_TESTV( !syscall( SYS_pivot_root, ".", "old-root" ) );
  FD_TESTV( !chdir( "/" ) );
  FD_TESTV( !umount2( "old-root", MNT_DETACH ) );
  FD_TESTV( !rmdir( "old-root" ) );
}

static void
check_fds( ulong allow_fds_sz,
           int * allow_fds ) {
  DIR * dir = opendir( "/proc/self/fd" );
  FD_TESTV( dir );
  int dirfd1 = dirfd( dir );
  FD_TESTV( dirfd1 >= 0 );

  struct dirent *dp;

  int seen_fds[ 256 ] = {0};
  FD_TESTV( allow_fds_sz < 256 );

  while( ( dp = readdir( dir ) ) ) {
    char *end;
    long fd = strtol( dp->d_name, &end, 10 );
    FD_TESTV( fd < INT_MAX && fd > INT_MIN );
    if ( *end != '\0' ) {
      continue;
    }

    if( FD_LIKELY( fd == dirfd1 ) ) continue;

    int found = 0;
    for( ulong i=0; i<allow_fds_sz; i++ ) {
      if ( FD_LIKELY( fd==allow_fds[ i ] ) ) {
        seen_fds[ i ] = 1;
        found = 1;
        break;
      }
    }

    if( !found ) {
      char path[ PATH_MAX ];
      int len = snprintf( path, PATH_MAX, "/proc/self/fd/%ld", fd );
      FD_TEST( len>0 && len < PATH_MAX );
      char target[ PATH_MAX ];
      long count = readlink( path, target, PATH_MAX );
      if( FD_UNLIKELY( count < 0 ) ) FD_LOG_ERR(( "readlink(%s) failed (%i-%s)", target, errno, strerror( errno ) ));
      if( FD_UNLIKELY( count >= PATH_MAX ) ) FD_LOG_ERR(( "readlink(%s) returned truncated path", path ));
      target[ count ] = '\0';

      FD_LOG_ERR(( "unexpected file descriptor %ld open %s", fd, target ));
    }
  }

  for( ulong i=0; i<allow_fds_sz; i++ ) {
    if( FD_UNLIKELY( !seen_fds[ i ] ) ) {
      FD_LOG_ERR(( "allowed file descriptor %d not present", allow_fds[ i ] ));
    }
  }

  FD_TESTV( !closedir( dir ) );
}

static void
install_seccomp( ushort allow_syscalls_sz, long * allow_syscalls ) {
  FD_TEST( allow_syscalls_sz < 32 - 5 );

  struct sock_filter filter [ 128 ] = {
    /* validate architecture, load the arch number */
    BPF_STMT( BPF_LD | BPF_W | BPF_ABS, ( offsetof( struct seccomp_data, arch ) ) ),

    /* do not jump (and die) if the compile arch is neq the runtime arch.
       Otherwise, jump over the SECCOMP_RET_KILL_PROCESS statement. */
    BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, ARCH_NR, 1, 0 ),
    BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ALLOW ),

    /* verify that the syscall is allowed, oad the syscall */
    BPF_STMT( BPF_LD | BPF_W | BPF_ABS, ( offsetof( struct seccomp_data, nr ) ) ),
  };

  for( ulong i=0; i<allow_syscalls_sz; i++ ) {
    /* If the syscall does not match, jump over RET_ALLOW */
    struct sock_filter jmp = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint)allow_syscalls[i], 0, 1);
    struct sock_filter ret = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    filter[ 4+(i*2)   ] = jmp;
    filter[ 4+(i*2)+1 ] = ret;
  }

  /* none of the syscalls approved were matched: die */
  struct sock_filter kill = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  filter[ 4+(allow_syscalls_sz*2) ] = kill;

  FD_TESTV( 0 == prctl( PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0 ) );

  struct sock_fprog default_prog = {
    .len = (ushort)(5+(allow_syscalls_sz*2)),
    .filter = filter,
  };
  FD_TESTV( 0 == syscall( SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &default_prog ) );
}

static void
drop_capabilities( void ) {
  FD_TESTV( 0 == prctl (
    PR_SET_SECUREBITS,
    SECBIT_KEEP_CAPS_LOCKED | SECBIT_NO_SETUID_FIXUP |
      SECBIT_NO_SETUID_FIXUP_LOCKED | SECBIT_NOROOT |
      SECBIT_NOROOT_LOCKED | SECBIT_NO_CAP_AMBIENT_RAISE |
      SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED ) );

  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct   data[2] = { { 0 } };
  FD_TESTV( 0 == syscall( SYS_capset, &hdr, data ) );

  FD_TESTV( 0 == prctl( PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0 ) );
}

static void
userns_map( uint id, const char * map ) {
  char path[64];
  FD_TESTV( sprintf( path, "/proc/self/%s", map ) > 0);
  int fd = open( path, O_WRONLY );
  FD_TESTV( fd >= 0 );
  char line[64];
  FD_TESTV( sprintf( line, "0 %u 1\n", id ) > 0 );
  FD_TESTV( write( fd, line, strlen( line ) ) > 0 );
  FD_TESTV( !close( fd ) );
}

static void
deny_setgroups( void ) {
  int fd = open( "/proc/self/setgroups", O_WRONLY );
  FD_TESTV( fd >= 0 );
  FD_TESTV( write( fd, "deny", strlen( "deny" ) ) > 0 );
  FD_TESTV( !close( fd ) );
}

static void
unshare_user( uint uid, uint gid ) {
  FD_TESTV( !setresgid( gid, gid, gid ) );
  FD_TESTV( !setresuid( uid, uid, uid ) );
  FD_TESTV( !prctl( PR_SET_DUMPABLE, 1, 0, 0, 0 ) );

  FD_TESTV( !unshare( CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET ) );
  deny_setgroups();
  userns_map( uid, "uid_map" );
  userns_map( gid, "gid_map" );

  FD_TESTV( !prctl( PR_SET_DUMPABLE, 0, 0, 0, 0 ) );
  for ( int cap = 0; cap <= CAP_LAST_CAP; cap++ ) {
    FD_TESTV( !prctl( PR_CAPBSET_DROP, cap, 0, 0, 0 ) );
  }
}

/* Sandbox the current process by dropping all privileges and entering various
   restricted namespaces, but leave it able to make system calls. This should be
   done as a first step before later calling`install_seccomp`.

   You should call `unthreaded` before creating any threads in the process, and
   then install the seccomp profile afterwards. */
static void
sandbox_unthreaded( ulong allow_fds_sz,
                    int * allow_fds,
                    uint uid,
                    uint gid ) {
  check_fds( allow_fds_sz, allow_fds );
  unshare_user( uid, gid );
  struct rlimit limit = { .rlim_cur = 0, .rlim_max = 0 };
  FD_TESTV( !setrlimit( RLIMIT_NOFILE, &limit ));
  setup_mountns();
  drop_capabilities();
  secure_clear_environment();
}

void
fd_sandbox( uint   uid,
            uint   gid,
            ulong  allow_fds_sz,
            int *  allow_fds,
            ushort allow_syscalls_sz,
            long * allow_syscalls ) {
  sandbox_unthreaded( allow_fds_sz, allow_fds, uid, gid );
  install_seccomp( allow_syscalls_sz, allow_syscalls );
}
