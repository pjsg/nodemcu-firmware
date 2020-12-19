
#include <stdio.h>
#include <stdlib.h>
#include "platform.h"
#include "lfs.h"
#include "lfs_flashbd.h"
#include "lfs_main.h"

/*
 * With the intoduction of a unified FatFS and SPIFFS support (#1397), the LITTLEFS
 * interface is now abstracted through a uses a single LITTLEFS entry point
 * littlefs_realm() which returns a vfs_fs_fns object (as does myfatfs_realm()).
 * All other functions and data are static.
 *
 * Non-OS SDK V3.0 introduces a flash partition table (PT) and LITTLEFS has now been
 * updated to support this:
 *   -  SPIFFS limits search to the specifed SPIFFS0 address and size.
 *   -  Any headroom / offset from other partitions is reflected in the PT allocations.
 *   -  Unforced mounts will attempt to mount any valid SPIFSS found in this range
 *      (NodeMCU uses the SPIFFS_USE_MAGIC setting to make existing FS discoverable).
 *   -  Subject to the following, no offset or FS search is done.  The FS is assumed
 *      to be at the first valid location at the start of the partition.
 */

static lfs_t fs;
static lfs_flashbd_t flash_cfg;
static struct lfs_config cfg;
static int errcode;

#define SAVE_ERRCODE(res) do { \
	if (res < 0) errcode = -res; \
} while(0)

static void (*automounter)();

#define MASK_1MB (0x100000-1)
#define ALIGN (0x2000)

/*******************
 * Note that the W25Q32BV array is organized into 16,384 programmable pages of 256-bytes 
 * each. Up to 256 bytes can be programmed at a time.  Pages can be erased in groups of 
 * 16 (4KB sector erase), groups of 128 (32KB block erase), groups of 256 (64KB block 
 * erase) or the entire chip (chip erase). The W25Q32BV has 1,024 erasable sectors and 
 * 64 erasable blocks respectively. The small 4KB sectors allow for greater flexibility 
 * in applications that require data and parameter storage. 
 *
 * Returns  TRUE if FS was found.
 */
static bool littlefs_set_cfg(struct lfs_config *cfg, bool force_create) {
  uint32 pt_start, pt_size, pt_end;

  pt_size = platform_flash_get_partition (NODEMCU_LITTLEFS0_PARTITION, &pt_start);
  if (pt_size == 0) {
    return FALSE;
  }
  pt_end = pt_start + pt_size;

  memset(cfg, 0, sizeof(*cfg));
  memset(&flash_cfg, 0, sizeof(flash_cfg));

  cfg->read_size = INTERNAL_FLASH_READ_UNIT_SIZE;
  cfg->prog_size = INTERNAL_FLASH_WRITE_UNIT_SIZE;
  cfg->block_cycles = 1000;
  cfg->cache_size = 256;
  cfg->lookahead_size = 512;

  cfg->read_buffer = malloc(cfg->cache_size);
  cfg->prog_buffer = malloc(cfg->cache_size);
  cfg->lookahead_buffer = malloc(cfg->lookahead_size);

  cfg->read = lfs_flashbd_read;
  cfg->prog = lfs_flashbd_prog;
  cfg->erase = lfs_flashbd_erase;
  cfg->sync = lfs_flashbd_sync;
  cfg->block_size = INTERNAL_FLASH_SECTOR_SIZE;
  flash_cfg.phys_addr = (pt_start + ALIGN - 1) & ~(ALIGN - 1);
  flash_cfg.phys_size = (pt_end & ~(ALIGN - 1)) - flash_cfg.phys_addr;
  cfg->block_count = flash_cfg.phys_size / INTERNAL_FLASH_SECTOR_SIZE;
  cfg->context = &flash_cfg;
  cfg->name_max = FS_OBJ_NAME_LEN + 1;
 
  if (flash_cfg.phys_size < 6 * INTERNAL_FLASH_SECTOR_SIZE) {
    return FALSE;
  } 

#ifdef LITTLEFS_USE_MAGIC_LENGTH
  if (!force_create) {
    int size = SPIFFS_probe_fs(cfg);

    if (size > 0 && size < cfg->phys_size) {
      NODE_DBG("Overriding size:%x\n",size);
      cfg->phys_size = size;
    }
    if (size <= 0) {
      return FALSE;
    }
  }
#endif

  NODE_DBG("littlefs set cfg block: %x  %x  %x  %x  %x  %x\n", pt_start, pt_end,
           flash_cfg.phys_size, flash_cfg.phys_addr, flash_cfg.phys_size, cfg->log_block_size);

  return TRUE;
}


static bool littlefs_mount(bool force_mount) {
  STARTUP_COUNT;
  if (!littlefs_set_cfg(&cfg, force_mount) && !force_mount) {
    return FALSE;
  }

  errcode = 0;

  int res = lfs_mount(&fs, &cfg);
  SAVE_ERRCODE(res);
  NODE_DBG("mount res: %d, %d\n", res, fs.err_code);
  STARTUP_COUNT;
  return res >= 0;
}

void littlefs_unmount() {
  lfs_unmount(&fs);
}

// FS formatting function
// Returns 1 if OK, 0 for error
int littlefs_format( void )
{
  lfs_unmount(&fs);

  if (!littlefs_set_cfg(&cfg, TRUE)) {
    return FALSE;
  }

  NODE_DBG("Formatting: size 0x%x, addr 0x%x\n", cfg.phys_size, cfg.phys_addr);

  int status = lfs_format(&fs, &cfg);
  SAVE_ERRCODE(status);

  return status < 0 ? 0 : littlefs_mount(FALSE);
}


// ***************************************************************************
// vfs API
// ***************************************************************************

#include <stdlib.h>
#include "vfs_int.h"

#define MY_LDRV_ID "FLASH"

// default current drive
static int is_current_drive = TRUE;

// forward declarations
static sint32_t littlefs_vfs_close( const struct vfs_file *fd );
static sint32_t littlefs_vfs_read( const struct vfs_file *fd, void *ptr, size_t len );
static sint32_t littlefs_vfs_write( const struct vfs_file *fd, const void *ptr, size_t len );
static sint32_t littlefs_vfs_lseek( const struct vfs_file *fd, sint32_t off, int whence );
static sint32_t littlefs_vfs_eof( const struct vfs_file *fd );
static sint32_t littlefs_vfs_tell( const struct vfs_file *fd );
static sint32_t littlefs_vfs_flush( const struct vfs_file *fd );
static uint32_t littlefs_vfs_size( const struct vfs_file *fd );
static sint32_t littlefs_vfs_ferrno( const struct vfs_file *fd );

static sint32_t  littlefs_vfs_closedir( const struct vfs_dir *dd );
static sint32_t  littlefs_vfs_readdir( const struct vfs_dir *dd, struct vfs_stat *buf );

static vfs_vol  *littlefs_vfs_mount( const char *name, int num );
static vfs_file *littlefs_vfs_open( const char *name, const char *mode );
static vfs_dir  *littlefs_vfs_opendir( const char *name );
static sint32_t  littlefs_vfs_stat( const char *name, struct vfs_stat *buf );
static sint32_t  littlefs_vfs_mkdir( const char *name );
static sint32_t  littlefs_vfs_remove( const char *name );
static sint32_t  littlefs_vfs_rename( const char *oldname, const char *newname );
static sint32_t  littlefs_vfs_fsinfo( uint32_t *total, uint32_t *used );
static sint32_t  littlefs_vfs_fscfg( uint32_t *phys_addr, uint32_t *phys_size );
static sint32_t  littlefs_vfs_format( void );
static sint32_t  littlefs_vfs_errno( void );
static void      littlefs_vfs_clearerr( void );

static sint32_t littlefs_vfs_umount( const struct vfs_vol *vol );

// ---------------------------------------------------------------------------
// function tables
//
static vfs_fs_fns littlefs_fs_fns = {
  .mount    = littlefs_vfs_mount,
  .open     = littlefs_vfs_open,
  .opendir  = littlefs_vfs_opendir,
  .stat     = littlefs_vfs_stat,
  .remove   = littlefs_vfs_remove,
  .rename   = littlefs_vfs_rename,
  .mkdir    = littlefs_vfs_mkdir,
  .fsinfo   = littlefs_vfs_fsinfo,
  .fscfg    = littlefs_vfs_fscfg,
  .format   = littlefs_vfs_format,
  .chdrive  = NULL,
  .chdir    = NULL,
  .ferrno   = littlefs_vfs_errno,
  .clearerr = littlefs_vfs_clearerr
};

static vfs_file_fns littlefs_file_fns = {
  .close     = littlefs_vfs_close,
  .read      = littlefs_vfs_read,
  .write     = littlefs_vfs_write,
  .lseek     = littlefs_vfs_lseek,
  .eof       = littlefs_vfs_eof,
  .tell      = littlefs_vfs_tell,
  .flush     = littlefs_vfs_flush,
  .size      = littlefs_vfs_size,
  .ferrno    = littlefs_vfs_ferrno
};

static vfs_dir_fns littlefs_dd_fns = {
  .close     = littlefs_vfs_closedir,
  .readdir   = littlefs_vfs_readdir
};


// ---------------------------------------------------------------------------
// specific struct extensions
//
struct myvfs_file {
  struct vfs_file vfs_file;
  lfs_file_t lfs_file;
  struct lfs_file_config lfs_file_config;
  char buffer[0];
};

struct myvfs_dir {
  struct vfs_dir vfs_dir;
  lfs_dir_t lfs_dir;
};


// ---------------------------------------------------------------------------
// volume functions
//
static sint32_t littlefs_vfs_umount( const struct vfs_vol *vol ) {
  // not implemented

  return VFS_RES_ERR;
}


// ---------------------------------------------------------------------------
// dir functions
//
#define GET_DIR(descr) \
  const struct myvfs_dir *mydd = (const struct myvfs_dir *)descr; \
  lfs_dir_t *dir = (lfs_dir_t *)&(mydd->lfs_dir);

static sint32_t littlefs_vfs_closedir( const struct vfs_dir *dd ) {
  GET_DIR(dd);

  sint32_t res = lfs_dir_close(&fs, dir);
  SAVE_ERRCODE(res);

  // free descriptor memory
  free( (void *)dd );
}

static sint32_t littlefs_vfs_readdir( const struct vfs_dir *dd, struct vfs_stat *buf ) {
  GET_DIR(dd);
  struct lfs_info info;

  while (1) {
    if (lfs_dir_read( &fs, dir, &info )) {
      if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
        continue;
      }
      memset( buf, 0, sizeof( struct vfs_stat ) );

      buf->is_dir = info.type == LFS_TYPE_DIR;

      // copy entries to  item
      // fill in supported stat entries
      strncpy( buf->name, info.name, FS_OBJ_NAME_LEN+1 );
      buf->name[FS_OBJ_NAME_LEN] = '\0';
      buf->size = info.size;
      return VFS_RES_OK;
    } else {
      break;
    }
  }

  return VFS_RES_ERR;
}


// ---------------------------------------------------------------------------
// file functions
//
#define GET_FILE(descr) \
  const struct myvfs_file *myfd = (const struct myvfs_file *)descr; \
  lfs_file_t *file = (lfs_file_t *) &myfd->lfs_file;

static sint32_t littlefs_vfs_close( const struct vfs_file *fd ) {
  GET_FILE(fd);

  sint32_t res = lfs_file_close( &fs, file );
  SAVE_ERRCODE(res);

  // free descriptor memory
  free( (void *)fd );

  return res;
}

static sint32_t littlefs_vfs_read( const struct vfs_file *fd, void *ptr, size_t len ) {
  GET_FILE(fd);

  sint32_t n = lfs_file_read( &fs, file, ptr, len );
  SAVE_ERRCODE(n);

  return n >= 0 ? n : VFS_RES_ERR;
}

static sint32_t littlefs_vfs_write( const struct vfs_file *fd, const void *ptr, size_t len ) {
  GET_FILE(fd);

  sint32_t n = lfs_file_write( &fs, file, (void *)ptr, len );
  SAVE_ERRCODE(n);

  return n >= 0 ? n : VFS_RES_ERR;
}

static sint32_t littlefs_vfs_lseek( const struct vfs_file *fd, sint32_t off, int whence ) {
  GET_FILE(fd);
  int lfs_whence;

  switch (whence) {
  default:
  case VFS_SEEK_SET:
    lfs_whence = LFS_SEEK_SET;
    break;
  case VFS_SEEK_CUR:
    lfs_whence = LFS_SEEK_CUR;
    break;
  case VFS_SEEK_END:
    lfs_whence = LFS_SEEK_END;
    break;
  }

  sint32_t res = lfs_file_seek( &fs, file, off, lfs_whence );
  SAVE_ERRCODE(res);
  return res >= 0 ? res : VFS_RES_ERR;
}

static sint32_t littlefs_vfs_eof( const struct vfs_file *fd ) {
  GET_FILE(fd);

  return lfs_file_tell(&fs, file) == lfs_file_size(&fs, file);
}

static sint32_t littlefs_vfs_tell( const struct vfs_file *fd ) {
  GET_FILE(fd);

  return lfs_file_tell( &fs, file );
}

static sint32_t littlefs_vfs_flush( const struct vfs_file *fd ) {
  GET_FILE(fd);

  return lfs_file_sync( &fs, file ) >= 0 ? VFS_RES_OK : VFS_RES_ERR;
}

static uint32_t littlefs_vfs_size( const struct vfs_file *fd ) {
  GET_FILE(fd);

  return lfs_file_size(&fs, file);
}

static sint32_t littlefs_vfs_ferrno( const struct vfs_file *fd ) {
  return errcode;
}


static int fs_mode2flag(const char *mode){
  if(strlen(mode)==1){
  	if(strcmp(mode,"w")==0)
  	  return LFS_O_WRONLY|LFS_O_CREAT|LFS_O_TRUNC;
  	else if(strcmp(mode, "r")==0)
  	  return LFS_O_RDONLY;
  	else if(strcmp(mode, "a")==0)
  	  return LFS_O_WRONLY|LFS_O_CREAT|LFS_O_APPEND;
  	else
  	  return LFS_O_RDONLY;
  } else if (strlen(mode)==2){
  	if(strcmp(mode,"r+")==0)
  	  return LFS_O_RDWR;
  	else if(strcmp(mode, "w+")==0)
  	  return LFS_O_RDWR|LFS_O_CREAT|LFS_O_TRUNC;
  	else if(strcmp(mode, "a+")==0)
  	  return LFS_O_RDWR|LFS_O_CREAT|LFS_O_APPEND;
  	else
  	  return LFS_O_RDONLY;
  } else {
  	return LFS_O_RDONLY;
  }
}

// ---------------------------------------------------------------------------
// filesystem functions
//
static vfs_file *littlefs_vfs_open( const char *name, const char *mode ) {
  struct myvfs_file *fd;
  int flags = fs_mode2flag( mode );

  if (fd = (struct myvfs_file *)malloc( sizeof( struct myvfs_file ) + cfg.cache_size )) {
    memset(fd, 0, sizeof(*fd));
    fd->lfs_file_config.buffer = &fd->buffer;
    sint32_t res = lfs_file_opencfg( &fs, &fd->lfs_file, name, flags, &fd->lfs_file_config );
    if (0 <= res) {
      fd->vfs_file.fs_type = VFS_FS_LFS;
      fd->vfs_file.fns     = &littlefs_file_fns;
      return (vfs_file *)fd;
    } else {
      SAVE_ERRCODE(res);
      free( fd );
    }
  }

  return NULL;
}

static vfs_dir *littlefs_vfs_opendir( const char *name ){
  struct myvfs_dir *dd;

  if (dd = (struct myvfs_dir *)malloc( sizeof( struct myvfs_dir ) )) {
    memset(dd, 0, sizeof(*dd));
    if (!lfs_dir_open( &fs, &dd->lfs_dir, name )) {
      dd->vfs_dir.fs_type = VFS_FS_LFS;
      dd->vfs_dir.fns     = &littlefs_dd_fns;
      return (vfs_dir *)dd;
    } else {
      free( dd );
    }
  }

  return NULL;
}

static sint32_t littlefs_vfs_stat( const char *name, struct vfs_stat *buf ) {
  struct lfs_info info;
  sint32_t err;

  if (0 <= (err = lfs_stat( &fs, name, &info ))) {
    memset( buf, 0, sizeof( struct vfs_stat ) );

    // fill in supported stat entries
    strncpy( buf->name, info.name, FS_OBJ_NAME_LEN+1 );
    buf->name[FS_OBJ_NAME_LEN] = '\0';
    buf->size = info.size;

    return VFS_RES_OK;
  } else {
    SAVE_ERRCODE(err);
    return VFS_RES_ERR;
  }
}

static sint32_t littlefs_vfs_remove( const char *name ) {
  sint32_t res = lfs_remove( &fs, name );
  SAVE_ERRCODE(res);
  return res;
}

static sint32_t littlefs_vfs_rename( const char *oldname, const char *newname ) {
  sint32_t res = lfs_rename( &fs, oldname, newname );
  SAVE_ERRCODE(res);
  return res;
}

static sint32_t littlefs_vfs_mkdir( const char *name ) {
  sint32_t res = lfs_mkdir( &fs, name );
  SAVE_ERRCODE(res);
  return res;
}

static sint32_t littlefs_vfs_fsinfo( uint32_t *total, uint32_t *used ) {
  lfs_ssize_t used_blocks = lfs_fs_size(&fs);

  *total = cfg.block_size * cfg.block_count;
  *used = cfg.block_size * used_blocks;
  if (*used > *total) {
    *used = *total;
  }
  return VFS_RES_OK;
}

static sint32_t littlefs_vfs_fscfg( uint32_t *phys_addr, uint32_t *phys_size ) {
  *phys_addr = flash_cfg.phys_addr;
  *phys_size = flash_cfg.phys_size;
  return VFS_RES_OK;
}

static vfs_vol  *littlefs_vfs_mount( const char *name, int num ) {
  // volume descriptor not supported, just return TRUE / FALSE
  return littlefs_mount(FALSE) ? (vfs_vol *)1 : NULL;
}

static sint32_t littlefs_vfs_format( void ) {
  return littlefs_format();
}

static sint32_t littlefs_vfs_errno( void ) {
  return errcode;
}

static void littlefs_vfs_clearerr( void ) {
  errcode = 0;
}

// The callback will be called on the first file operation
void littlefs_set_automount(void (*mounter)()) {
  automounter = mounter;
}

// ---------------------------------------------------------------------------
// VFS interface functions
//

vfs_fs_fns *littlefs_realm( const char *inname, char **outname, int set_current_drive ) {
  if (automounter) {
    void (*mounter)() = automounter;
    automounter = NULL;
    mounter();
  }
  if (inname[0] == '/') {
    // logical drive is specified, check if it's our id
    if (0 == strncmp(inname + 1, MY_LDRV_ID, sizeof(MY_LDRV_ID)-1)) {
      *outname = (char *)(inname + sizeof(MY_LDRV_ID));
      if (*outname[0] == '/') {
        // skip leading /
        (*outname)++;
      }

      if (set_current_drive) is_current_drive = TRUE;
      return &littlefs_fs_fns;
    }
  } else {
    // no logical drive in patchspec, are we current drive?
    if (is_current_drive) {
      *outname = (char *)inname;
      return &littlefs_fs_fns;
    }
  }

  if (set_current_drive) is_current_drive = FALSE;
  return NULL;
} 

void __assert_func(const char *file, int line, const char *func, const char *expr) {
    printf("Assert %s in %s at %s:%d\n", expr, func, file, line);
    while (1) {}
};

