#include "configure.h"

#define NAME "sysctl"

#include <stdio.h>
#include <linux/capability.h>

static void
init_perm( security_t *     security,
           config_t * const config ) {
  (void)config;
  check_cap( security, NAME, CAP_SYS_ADMIN, "set kernel parameters in `/proc/sys`" );
}

static const char * params[] = {
  "/proc/sys/net/core/rmem_max",
  "/proc/sys/net/core/rmem_default",
  "/proc/sys/net/core/wmem_max",
  "/proc/sys/net/core/wmem_default",
  "/proc/sys/vm/max_map_count",
};

static uint limits[] = {
  134217728,
  134217728,
  134217728,
  134217728,
  1000000,
};

/* These sysctl limits are needed for the Solana Labs client, not Firedancer.
   We set them on their behalf to make configuration easier for users. */
static void
init( config_t * const config ) {
  (void)config;
  for( ulong i=0; i<sizeof( params ) / sizeof( params[ 0 ] ); i++ ) {
    uint param = read_uint_file( params[ i ] );
    if( FD_UNLIKELY( param < limits[ i ] ) )
      write_uint_file( params[ i ], limits[ i ] );
  }
}

static configure_result_t
check( config_t * const config ) {
  (void)config;
  for( ulong i=0; i<sizeof( params ) / sizeof( params[ 0 ] ); i++ ) {
    uint param = read_uint_file( params[ i ] );
    if( FD_UNLIKELY( param < limits[ i ] ) )
      NOT_CONFIGURED( "kernel parameter `%s` is too low (%u < %lu)", params[ i ], param, limits[ i ] );
  }

  CONFIGURE_OK();
}

configure_stage_t sysctl = {
  .name            = NAME,
  .always_recreate = 0,
  .enabled         = NULL,
  .init_perm       = init_perm,
  .fini_perm       = NULL,
  .init            = init,
  .fini            = NULL,
  .check           = check,
};

#undef NAME
