#include "configure.h"

#include "../../../tango/fd_tango.h"
#include "../../../tango/quic/fd_quic.h"
#include "../../../tango/xdp/fd_xsk_aio.h"

#include <sys/stat.h>
#include <linux/capability.h>

#define NAME "workspace"

static void
init_perm( security_t *     security,
           config_t * const config ) {
  if( FD_UNLIKELY( config->development.netns.enabled ) )
    check_cap( security, NAME, CAP_SYS_ADMIN, "enter a network namespace" );

  ulong limit = memlock_max_bytes( config );
  check_res( security, NAME, RLIMIT_MEMLOCK, limit, "increase `RLIMIT_MEMLOCK` to lock the workspace in memory" );
}

#define INSERTER(arg, align, footprint, new) do {                                   \
    fd_wksp_t * wksp  = fd_wksp_containing( pod );                                  \
    ulong       gaddr = fd_wksp_alloc( wksp, ( align ), (footprint ), 1 );          \
    void *      shmem = fd_wksp_laddr( wksp, gaddr );                               \
    if( FD_UNLIKELY( !shmem ) ) FD_LOG_ERR(( "failed to allocate memory" ));        \
    (void)shmem;                                                                    \
    if( FD_UNLIKELY( !( new ) ) ) FD_LOG_ERR(( "failed to initialize workspace" )); \
    char name[ PATH_MAX ];                                                          \
    va_list args;                                                                   \
    va_start( args, arg );                                                          \
    vsnprintf( name, PATH_MAX, fmt, args );                                         \
    va_end( args );                                                                 \
    char buffer[ FD_WKSP_CSTR_MAX ];                                                \
    if( FD_UNLIKELY( !fd_wksp_cstr( wksp, gaddr, buffer ) ) )                       \
      FD_LOG_ERR(( "failed to get wksp gaddr for pod entry `%s`", name ));          \
    if( FD_UNLIKELY( !fd_pod_insert_cstr( pod, name, buffer ) ) )                   \
      FD_LOG_ERR(( "failed to insert value into pod for `%s`", name ));             \
  } while( 0 )

static void cnc( void * pod, char * fmt, ... ) {
  INSERTER( fmt,
            fd_cnc_align    (                                ),
            fd_cnc_footprint( 4032                           ),
            fd_cnc_new      ( shmem, 4032, 0, fd_tickcount() ) );
}

static void mcache( void * pod, char * fmt, ulong depth, ... ) {
  INSERTER( depth,
            fd_mcache_align    (                    ),
            fd_mcache_footprint( depth, 0           ),
            fd_mcache_new      ( shmem, depth, 0, 0 ) );
}

static void dcache( void * pod, char * fmt, ulong mtu, ulong depth, ulong app_sz, ... ) {
  ulong data_sz = fd_dcache_req_data_sz( mtu, depth, 1, 1 );
  INSERTER( app_sz,
            fd_dcache_align    (                          ),
            fd_dcache_footprint( data_sz, app_sz          ),
            fd_dcache_new      ( shmem,   data_sz, app_sz ) );
}

static void fseq( void * pod, char * fmt, ... ) {
  INSERTER( fmt,
            fd_fseq_align    (          ),
            fd_fseq_footprint(          ),
            fd_fseq_new      ( shmem, 0 ) );
}

static void tcache( void * pod, char * fmt, ulong depth, ... ) {
  INSERTER( depth,
            fd_tcache_align    (                 ),
            fd_tcache_footprint( depth, 0        ),
            fd_tcache_new      ( shmem, depth, 0 ) );
}

static void quic( void * pod, char * fmt, fd_quic_limits_t * limits, ... ) {
  INSERTER( limits,
            fd_quic_align    (               ),
            fd_quic_footprint( limits        ),
            fd_quic_new      ( shmem, limits ) );
}

static void xsk( void * pod, char * fmt, ulong frame_sz, ulong rx_depth, ulong tx_depth, ... ) {
  INSERTER( tx_depth,
            fd_xsk_align    (                                                            ),
            fd_xsk_footprint( frame_sz, rx_depth, rx_depth, tx_depth, tx_depth           ),
            fd_xsk_new      ( shmem,    frame_sz, rx_depth, rx_depth, tx_depth, tx_depth ) );
}

static void xsk_aio( void * pod, char * fmt, ulong tx_depth, ulong batch_count, ... ) {
  INSERTER( batch_count,
            fd_xsk_aio_align    (                                 ),
            fd_xsk_aio_footprint( tx_depth, batch_count           ),
            fd_xsk_aio_new      ( shmem,    tx_depth, batch_count ) );
}

FD_FN_UNUSED static void alloc( void * pod, char * fmt, ulong align, ulong sz, ... ) {
  INSERTER( sz, align, sz, 1 );
}

#define VALUE( type, value ) do {                                 \
    char name[ PATH_MAX ];                                        \
    va_list args;                                                 \
    va_start( args, value );                                      \
    vsnprintf( name, PATH_MAX, fmt, args );                       \
    va_end( args );                                               \
    if( FD_UNLIKELY( !fd_pod_insert_##type( pod, fmt, value ) ) ) \
      FD_LOG_ERR(( "failed to initialize workspace" ));           \
  } while( 0 )

static void ulong1( void * pod, char * fmt, ulong value, ... ) {
  VALUE( ulong, value );
}

static void uint1( void * pod, char * fmt, uint value, ... ) {
  VALUE( uint, value );
}

/* need a dummy argument so we can locate va_args (value would be default
   promoted) */
static void ushort1( void * pod, char * fmt, ushort value, ulong dummy, ... ) {
  char name[ PATH_MAX ];
  va_list args;
  va_start( args, dummy );
  vsnprintf( name, PATH_MAX, fmt, args );
  va_end( args );
  if( FD_UNLIKELY( !fd_pod_insert_ushort( pod, fmt, value ) ) )
    FD_LOG_ERR(( "failed to initialize workspace" ));
}

static void buf( void * pod, char * fmt, void * value, ulong sz, ... ) {
  char name[ PATH_MAX ];
  va_list args;
  va_start( args, sz );
  vsnprintf( name, PATH_MAX, fmt, args );
  va_end( args );
  if( FD_UNLIKELY( !fd_pod_insert_buf( pod, fmt, value, sz ) ) )
    FD_LOG_ERR(( "failed to initialize workspace" ));
}

#define WKSP_BEGIN( config, wksp1, cpu_idx ) do {                                                                       \
    char name[ FD_WKSP_CSTR_MAX ];                                                                                      \
    workspace_name( config, wksp1, name );                                                                              \
    /* one normal page on the desired tile cpu */                                                                       \
    ulong sub_page_cnt[ 1 ] = { wksp1->num_pages };                                                                     \
    ulong sub_cpu_idx [ 1 ] = { cpu_idx };                                                                              \
    int result = fd_wksp_new_named( name, wksp1->page_size, 1, sub_page_cnt, sub_cpu_idx, S_IRUSR | S_IWUSR, 0U, 0UL ); \
    if( FD_UNLIKELY( result ) ) FD_LOG_ERR(( "fd_wksp_new_named failed (%i-%s)", result, fd_wksp_strerror( result ) )); \
    fd_wksp_t * wksp  = fd_wksp_attach( name );                                                                         \
    /* pod must always be the first thing we allocate in the workspace,                                                 \
       so we know how to find it later (it will be at gaddr_low). */                                                    \
    ulong       gaddr = fd_wksp_alloc( wksp, fd_pod_align(), fd_pod_footprint( 16384 ), 1 );                            \
    void *      pod   = fd_wksp_laddr( wksp, gaddr );                                                                   \
    if( FD_UNLIKELY( !fd_pod_new( pod, 16384 ) ) ) FD_LOG_ERR(( "failed to initialize workspace" ));

#define WKSP_END()                                                                                        \
    char pod_str[ FD_WKSP_CSTR_MAX ];                                                                     \
    if( FD_UNLIKELY( !fd_wksp_cstr( wksp, gaddr, pod_str ) ) )                                            \
      FD_LOG_ERR(( "failed to initialize workspace" ));                                                   \
    fd_wksp_detach( wksp );                                                                               \
  } while(0);

static void
workspace_name( config_t * const config,
                workspace_config_t * wksp,
                char out[ FD_WKSP_CSTR_MAX ] ) {
  snprintf1( out, PATH_MAX, "%s_%s%lu.wksp", config->name, wksp->name, wksp->kind_idx );
}

static void
workspace_path( config_t * const config,
                workspace_config_t * wksp,
                char out[ PATH_MAX ] ) {
  char * mount_path;
  if( wksp->page_size == FD_SHMEM_GIGANTIC_PAGE_SZ ) mount_path = config->shmem.gigantic_page_mount_path;
  else if( wksp->page_size == FD_SHMEM_HUGE_PAGE_SZ ) mount_path = config->shmem.huge_page_mount_path;
  else FD_LOG_ERR(( "invalid page size %lu for workspace %s", wksp->page_size, wksp->name ));

  char name[ FD_WKSP_CSTR_MAX ];
  workspace_name( config, wksp, name );
  snprintf1( out, PATH_MAX, "%s/%s", mount_path, name );
}

static void
init( config_t * const config ) {
  /* enter network namespace for bind. this is only needed for a check
     that the interface exists.. we can probably skip that */
  if( FD_UNLIKELY( config->development.netns.enabled ) )  {
    enter_network_namespace( config->tiles.quic.interface );
  }

  /* switch to non-root uid/gid for workspace creation. permissions checks still done as root. */
  gid_t gid = getgid();
  uid_t uid = getuid();
  if( FD_LIKELY( gid == 0 && setegid( config->gid ) ) )
    FD_LOG_ERR(( "setegid() failed (%i-%s)", errno, strerror( errno ) ));
  if( FD_LIKELY( uid == 0 && seteuid( config->uid ) ) )
    FD_LOG_ERR(( "seteuid() failed (%i-%s)", errno, strerror( errno ) ));

  fd_quic_limits_t limits = {
    .conn_cnt                                      = config->tiles.quic.max_concurrent_connections,
    .handshake_cnt                                 = config->tiles.quic.max_concurrent_handshakes,
    .conn_id_cnt                                   = config->tiles.quic.max_concurrent_connection_ids_per_connection,
    .conn_id_sparsity                              = 0.0,
    .inflight_pkt_cnt                              = config->tiles.quic.max_inflight_quic_packets,
    .tx_buf_sz                                     = config->tiles.quic.tx_buf_size,
    .stream_cnt[ FD_QUIC_STREAM_TYPE_BIDI_CLIENT ] = 0,
    .stream_cnt[ FD_QUIC_STREAM_TYPE_BIDI_SERVER ] = 0,
    .stream_cnt[ FD_QUIC_STREAM_TYPE_UNI_CLIENT  ] = config->tiles.quic.max_concurrent_streams_per_connection,
    .stream_cnt[ FD_QUIC_STREAM_TYPE_UNI_SERVER  ] = 0,
  };

  for( ulong j=0; j<config->shmem.workspaces_cnt; j++ ) {
    workspace_config_t * wksp1 = &config->shmem.workspaces[ j ];
    WKSP_BEGIN( config, wksp1, 0 );

    switch( wksp1->kind ) {
      case wksp_quic_verify:
        for( ulong i=0; i<config->layout.verify_tile_count; i++ ) {
          mcache( pod, "mcache%lu", config->tiles.verify.receive_buffer_size, i );
          dcache( pod, "dcache%lu", config->tiles.verify.mtu, config->tiles.verify.receive_buffer_size, config->tiles.verify.receive_buffer_size * 32, i );
          fseq  ( pod, "fseq%lu", i );
        }
        break;
      case wksp_verify_dedup:
        ulong1( pod, "cnt", config->layout.verify_tile_count );
        for( ulong i=0; i<config->layout.verify_tile_count; i++ ) {
          mcache( pod, "mcache%lu", config->tiles.verify.receive_buffer_size, i );
          dcache( pod, "dcache%lu", config->tiles.verify.mtu, config->tiles.verify.receive_buffer_size, 0, i );
          fseq  ( pod, "fseq%lu",   i );
        }
        break;
      case wksp_dedup_pack:
        mcache( pod, "mcache", config->tiles.verify.receive_buffer_size );
        fseq  ( pod, "fseq" );
        break;
      case wksp_pack_bank:
        ulong1( pod, "num_tiles", config->layout.bank_tile_count );
        for( ulong i=0; i<config->layout.bank_tile_count; i++ ) {
          mcache( pod, "mcache%lu", config->tiles.bank.receive_buffer_size, i );
          dcache( pod, "dcache%lu", USHORT_MAX, config->layout.bank_tile_count * (ulong)config->tiles.bank.receive_buffer_size, 0, i );
          fseq  ( pod, "fseq%lu", i );
        }
        break;
      case wksp_bank_shred:
        for( ulong i=0; i<config->layout.bank_tile_count; i++ ) {
          mcache( pod, "mcache%lu", config->tiles.bank.receive_buffer_size, i );
          dcache( pod, "dcache%lu", USHORT_MAX, config->layout.bank_tile_count * (ulong)config->tiles.bank.receive_buffer_size, 0, i );
          fseq  ( pod, "fseq%lu", i );
        }
        break;
      case wksp_quic:
        cnc    ( pod, "cnc" );
        quic   ( pod, "quic",    &limits );
        xsk    ( pod, "xsk",     2048, config->tiles.quic.xdp_rx_queue_size, config->tiles.quic.xdp_tx_queue_size );
        xsk_aio( pod, "xsk_aio", config->tiles.quic.xdp_tx_queue_size, config->tiles.quic.xdp_aio_depth );

        char const * quic_xsk_gaddr = fd_pod_query_cstr( pod, "xsk", NULL );
        void *       shmem          = fd_wksp_map      ( quic_xsk_gaddr );
        if( FD_UNLIKELY( !fd_xsk_bind( shmem, config->name, config->tiles.quic.interface, (uint)wksp1->kind_idx ) ) )
          FD_LOG_ERR(( "failed to bind xsk for quic tile %lu", wksp1->kind_idx ));
        fd_wksp_unmap( shmem );

        uint1  ( pod, "ip_addr",                    config->tiles.quic.ip_addr );
        ushort1( pod, "listen_port",                config->tiles.quic.listen_port, 0 );
        buf    ( pod, "src_mac_addr",               config->tiles.quic.mac_addr, 6 );
        ulong1 ( pod, "idle_timeout_ms",            1000 );
        ulong1 ( pod, "initial_rx_max_stream_data", 1<<15 );
        break;
      case wksp_verify:
        cnc( pod, "cnc" );
        break;
      case wksp_dedup:
        cnc   ( pod, "cnc",    pod, wksp );
        tcache( pod, "tcache", config->tiles.dedup.signature_cache_size );
        break;
      case wksp_pack:
        cnc   ( pod, "cnc" );
        ulong1( pod, "min-gap", config->layout.bank_tile_count - 1UL        );
        ulong1( pod, "depth",   config->tiles.pack.max_pending_transactions );
        break;
      case wksp_bank:
        cnc   ( pod, "cnc" );
    }

    WKSP_END();
  }

  if( FD_UNLIKELY( seteuid( uid ) ) ) FD_LOG_ERR(( "seteuid() failed (%i-%s)", errno, strerror( errno ) ));
  if( FD_UNLIKELY( setegid( gid ) ) ) FD_LOG_ERR(( "setegid() failed (%i-%s)", errno, strerror( errno ) ));
}

static void
fini( config_t * const config ) {
  for( ulong i=0; i<config->shmem.workspaces_cnt; i++ ) {
    workspace_config_t * wksp = &config->shmem.workspaces[ i ];

    char path[ PATH_MAX ];
    workspace_path( config, wksp, path );

    struct stat st;
    int result = stat( path, &st );
    if( FD_LIKELY( !result ) ) {
      char name[ FD_WKSP_CSTR_MAX ];
      workspace_name( config, wksp, name );
      int err = fd_wksp_delete_named( name );
      if( FD_UNLIKELY( err ) ) FD_LOG_ERR(( "fd_wksp_delete_named failed (%i-%s)", err, fd_wksp_strerror( err ) ));
    }
    else if( FD_LIKELY( result && errno == ENOENT ) ) continue;
    else FD_LOG_ERR(( "stat failed when trying to delete wksp `%s` (%i-%s)", path, errno, strerror( errno ) ));
  }
}

static configure_result_t
check( config_t * const config ) {
  for( ulong i=0; i<config->shmem.workspaces_cnt; i++ ) {
    workspace_config_t * wksp = &config->shmem.workspaces[ i ];

    char path[ PATH_MAX ];
    workspace_path( config, wksp, path );

    struct stat st;
    int result = stat( path, &st );
    if( FD_LIKELY( !result ) ) PARTIALLY_CONFIGURED( "workspace `%s` exists", path );
    else if( FD_LIKELY( result && errno == ENOENT ) ) continue;
    else PARTIALLY_CONFIGURED( "error reading `%s` (%i-%s)", path, errno, strerror( errno ) );
  }

  NOT_CONFIGURED( "no workspaces files found" );
}

configure_stage_t workspace = {
  .name            = NAME,
  /* we can't really verify if a frank workspace has been set up
     correctly, so if we are running it we just recreate it every time */
  .always_recreate = 1,
  .enabled         = NULL,
  .init_perm       = init_perm,
  .fini_perm       = NULL,
  .init            = init,
  .fini            = fini,
  .check           = check,
};

#undef NAME
