/*
 * Code not otherwise copyrighted is Copyright (C) 2010-2011 Dan Reif/BlackMesh Managed Hosting.
 * This is version 0.4a.
 * 
 * With the permission of Miklos Szeredi, the entirety of this file is exclusively licensed
 * under the GNU GPL, version 2 or later:
 *
 * DFUSE, (C) Dan Reif, Miklos Szeredi, and others, based on the Hello FS template by Miklos
 * Szeredi <miklos@szeredi.hu>.  This code and any resultant executables or libraries are
 * governed by the terms of the GPL published by the GNU project of the Free Software
 * Foundation, either version 2, or (at your option) the latest version available at
 * www.gnu.org at the time of its use or modification.
 */

#define FUSE_USE_VERSION 25

#ifdef __MACH__
  #define __DARWIN_64_BIT_INO_T 0
  #define _FILE_OFFSET_BITS 64
  #define __FreeBSD__ 10
  #define HAVE_DIRFD
  #define HAVE_FPATHCONF
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse_opt.h>
#include <syslog.h>
#include <signal.h>
#include <stdlib.h>
#include <stddef.h>
#include <dirent.h>
#include <unistd.h>

/** options for fuse_opt.h */
struct options {
    char *fastpath_path;
}options;

FILE *debug_fd( void );

#ifdef DEBUG
#define D(x,y) {fprintf(debug_fd(),(x),(y));fflush(debug_fd());}
#else
#define D(x,y) (void)0
#endif

// Stands for "DFuse Return Value".  In DFuse, implements --lazy-connect.  Here, it's just for fun.
#define DFRV(v) { return (v); }

/** macro to define options */
#define DFUSE_OPT_KEY(t, p, v) { t, offsetof(struct options, p), v }

/** keys for FUSE_OPT_ options */
enum
{
    KEY_VERSION,
    KEY_HELP,
    KEY_FOREGROUND
};

static struct fuse_opt dfuse_opts[] =
{
    DFUSE_OPT_KEY("-F %s", fastpath_path, 0),

    // #define FUSE_OPT_KEY(templ, key) { templ, -1U, key }
    FUSE_OPT_KEY("-V",			KEY_VERSION),
    FUSE_OPT_KEY("--version",		KEY_VERSION),
    FUSE_OPT_KEY("-h",			KEY_HELP),
    FUSE_OPT_KEY("--help",		KEY_HELP),
    FUSE_OPT_KEY("--foreground",	KEY_FOREGROUND),
    FUSE_OPT_KEY("-f",			KEY_FOREGROUND),
    FUSE_OPT_END
};

void usage( char **argv );

FILE *cached_debug_fd = NULL;

#ifdef VMALLOC
//No support for calloc, realloc... it's a hack, replace it with something better.

/* Compile with -DVMALLOC and -DDEBUG to enable.  Try something like:
 * grep -i alloc /tmp/fusedebug | awk '/Dealloc/ {print $2} /Alloc/ {print $7}' | sort | uniq -c | sort -rn
 * to parse it, though really what you want is more like:
 * grep -i alloc /tmp/fusedebug | awk '/Dealloc/ {print $2} /Alloc/ {print $7}' | sort | uniq -c | grep -vE '[02468] '
 * though that's not tested.  The first syntax found me the fact that I never free()'d encoded_prikey in
 * dfuse_jsonify_row.  Oops!
 */

void * dfuse_malloc( size_t s, long line )
{
    void * p = NULL;

    D("Allocating %d bytes at ", (s));
    D("line %ld: ",line);
    p = malloc(s);
    D("%p\n",p);
    return p;
}

void dfuse_free( void * p, long line )
{
    D("Deallocating %p at ", (p));
    D("line %ld.\n",line);
    free(p);
}

#define DFUSE_MALLOC(s) dfuse_malloc((s),__LINE__)
#define DFUSE_FREE(p) dfuse_free((p),__LINE__)

#else

#define DFUSE_MALLOC(s) malloc((s))
#define DFUSE_FREE(p) free((p))

#endif

static int dfuse_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
    switch( key )
    {
	case KEY_HELP:
	case KEY_VERSION:
	    return -1;
	    break; //For thoroughness
	case KEY_FOREGROUND:
	    if ( fuse_daemonize(1) )
	    {
		printf( "Failed to forestall daemonization.\n" );
		return -1;
	    }
	    return 0;
	    break;
	default:
	    return 1; //"1" in this case means "not my problem": typically, the mountpoint.  Possibly other gibberish, though.
    }

    return -1; //Now this, on the other hand, is indefensibly paranoid.
}

FILE *debug_fd( void )
{
    FILE *fp;

    if ( cached_debug_fd ) { return cached_debug_fd; }

    if ( !( cached_debug_fd = fp = fopen( "/tmp/fusedebug", "a" ) ) )
    {
	raise(SIGSEGV);
    }

    return fp;
}

static int dfuse_readlink(const char *path, char *linkbuf, size_t bufsize )
{
    if ( bufsize <= 0 )
    {
	DFRV(-EINVAL);
    }

    //libc function says strlen("blah"), but FUSE API docs say +1.
    if ( bufsize < strlen(options.fastpath_path) + strlen(path) + 1 )
    {
	DFRV(-ENOMEM);
    }

    strncpy(linkbuf,options.fastpath_path,bufsize);
    strncpy(linkbuf+strlen(options.fastpath_path),path,bufsize-strlen(linkbuf));

    DFRV(0);
}

static int dfuse_getattr(const char *path, struct stat *stbuf)
{
    int rv;
    char *fastpath_file;
    struct stat *fastpath_stat = NULL;

    if( !path || path[0] == '\0' || path[0] != '/' )
    {
	DFRV(-ENOENT);
    }

    if ( !( fastpath_stat = DFUSE_MALLOC( sizeof( struct stat ) ) ) )
    {
	DFRV(-ENOMEM);
    }

    memset(fastpath_stat, 0, sizeof(struct stat));

    if ( !( fastpath_file = DFUSE_MALLOC( strlen( options.fastpath_path ) + strlen( path ) + 1 ) ) )
    {
	DFUSE_FREE(fastpath_stat);
	DFRV(-ENOMEM);
    }

    strcpy( fastpath_file, options.fastpath_path );
    strcpy( fastpath_file+strlen(options.fastpath_path), path );

    rv = stat( fastpath_file, fastpath_stat );

    if ( rv == ENOENT || rv == ENOTDIR )
    {
	// Find it in a slower source, or else fail to find it.
	DFRV( -ENOENT );
    }
    else if ( rv != 0 )
    {
	DFUSE_FREE(fastpath_stat);
	DFUSE_FREE(fastpath_file);
	DFRV(-rv);
    }

    if ( fastpath_stat->st_mode & S_IFDIR )
    {
//	stbuf->st_mode = S_IFDIR | 0755;
//	stbuf->st_nlink = 2;

	D("%s is a directory\n", fastpath_file);
	D("%s is the path\n", path);
	D("%x is the stat\n", fastpath_stat->st_mode);

	memcpy( stbuf, fastpath_stat, sizeof( struct stat ) );

	DFUSE_FREE(fastpath_stat);
	DFUSE_FREE(fastpath_file);
	DFRV( 0 );
    }

    stbuf->st_nlink = 1;
    stbuf->st_mode |= S_IFLNK | 0777;
    stbuf->st_size = strlen( options.fastpath_path ) + strlen( path );

    DFUSE_FREE(fastpath_stat);
    DFUSE_FREE(fastpath_file);
    DFRV( 0 );
}

//dirent_buf_size courtesy of http://womble.decadent.org.uk/readdir_r-advisory.html
/* Calculate the required buffer size (in bytes) for directory       *
 * entries read from the given directory handle.  Return -1 if this  *
 * this cannot be done.                                              *
 *                                                                   *
 * This code does not trust values of NAME_MAX that are less than    *
 * 255, since some systems (including at least HP-UX) incorrectly    *
 * define it to be a smaller value.                                  *
 *                                                                   *
 * If you use autoconf, include fpathconf and dirfd in your          *
 * AC_CHECK_FUNCS list.  Otherwise use some other method to detect   *
 * and use them where available.                                     */

size_t dirent_buf_size(DIR * dirp)
{
    long name_max;
    size_t name_end;
#   if defined(HAVE_FPATHCONF) && defined(HAVE_DIRFD) \
       && defined(_PC_NAME_MAX)
        name_max = fpathconf(dirfd(dirp), _PC_NAME_MAX);
        if (name_max == -1)
#           if defined(NAME_MAX)
                name_max = (NAME_MAX > 255) ? NAME_MAX : 255;
#           else
                return (size_t)(-1);
#           endif
#   else
#       if defined(NAME_MAX)
            name_max = (NAME_MAX > 255) ? NAME_MAX : 255;
#       else
#           error "buffer size for readdir_r cannot be determined"
#       endif
#   endif
    name_end = (size_t)offsetof(struct dirent, d_name) + name_max + 1;
    return (name_end > sizeof(struct dirent)
            ? name_end : sizeof(struct dirent));
}


static int dfuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
    long fastpath_path_length = strlen( options.fastpath_path );
    DIR * dirp;
    struct dirent *entry;
    struct dirent *result;
    char * fastpath_file;
    size_t dirent_size;

    // Path must start with '/'.
    if(!path || path[0] != '/')
    {
	DFRV(-ENOENT);
    }

    if ( !( fastpath_file = malloc( fastpath_path_length + strlen(path) + 1 ) ) )
    {
	DFRV(-ENOMEM);
    }

    strcpy(fastpath_file,options.fastpath_path);
    strcpy(fastpath_file+fastpath_path_length,path);

    // If the target doesn't exist on the fastpath, check on the slowpath(s); if still no
    // luck, return -ENOENT.  -ENOTDIR might also be relevant, but seems potentially complicated
    // to implement (we'd need to test each directory component, unless we do this in a way that
    // allows us to capitalize on something else's ENOTDIR.  Let's see what happens....

//    filler(buf, ".", NULL, 0);
//    filler(buf, "..", NULL, 0);

    if ( !( dirp = opendir(fastpath_file) ) )
    {
	DFUSE_FREE(fastpath_file);
	DFRV(-ENOMEM); //That's at least one of the failure modes, per "man 3 opendir".
    }

    if ( ( dirent_size = dirent_buf_size(dirp) ) <= 0 )
    {
	DFUSE_FREE(fastpath_file);
	DFRV(-ENOMEM); //Blatantly wrong, but I don't know a better option.
    }

    entry = (struct dirent *) malloc(dirent_size);

    while ( !readdir_r(dirp,entry,&result) )
    {
/*	if ( !strcmp(result,".") || !strcmp(result,"..") )
	{
	    continue;
	}
*/
	if ( !result )
	{
	    break;
	}

	filler(buf, result->d_name, NULL, 0);
    }

    /*while ( ( sql_row = mysql_fetch_row( sql_res ) ) )
    {
	char * ptr;
	if ( (filler(buf, ptr = urlencode(sql_row[0], mysql_fetch_lengths( sql_res )[0]), NULL, 0) ) )
	{
	    //The "filler" func's buffer is full, but it's unclear what the right thing to do is here.
	    //Opt to fail (very) noisily.
	    raise(SIGSEGV);
	}
	DFUSE_FREE(ptr);
    }*/

    closedir(dirp); //Failure is an option, because we have no idea what to do with failure.
    DFUSE_FREE(fastpath_file);
    DFRV(0);
}

static int dfuse_open(const char *path, struct fuse_file_info *fi)
{
    if ( !path || path[0] == '\0' )
    {
	DFRV(-EIO);
    }

    // We intercept creat(2), so even in the case of a new file being written, open(2)
    // should never be called.
    DFRV(-EIO);
}

static int dfuse_read(const char *path, char *buf, size_t size, off_t offset,
			struct fuse_file_info *fi)
{
    // As above.
    DFRV(-EIO);
}

static struct fuse_operations dfuse_oper = {
    .getattr = dfuse_getattr,
    .readdir = dfuse_readdir,
//    .open = dfuse_open,
//    .read = dfuse_read,
    .readlink = dfuse_readlink,
};

int main(int argc, char *argv[])
{
    int rv = -1;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

//    printf( "Starting...\n" );

    /* clear structure that holds our options */
    memset(&options, 0, sizeof(struct options));

    if (fuse_opt_parse(&args, &options, dfuse_opts, dfuse_opt_proc))
    {
	usage(argv);
//	printf( "Error parsing options.\n" );
	/** error parsing options, or -h/--help */
	return -1;
    }

    if ( !options.fastpath_path || strlen(options.fastpath_path) < 0 )
    {
	printf( "Undefined critical variable.\n" );

	usage(argv);

	return -1;
    }

    // TODO: Stat fastpath_path, make sure it's a directory.
    // TODO: Ensure fastpath_path is a full, canonical path without a trailing "/", but with a starting "/".

    rv = fuse_main(args.argc, args.argv, &dfuse_oper);

    if (rv)
    {
	printf("\n");
    }

    /** free arguments */
    fuse_opt_free_args(&args);

    return rv;
}

void usage( char **argv )
{
    printf( "Usage: %s <options> <mountpoint>\n"
	"  -F, --fastsource: the local, nominal storage directory\n"
//	Foreground doesn't seem to work properly at the moment; we'll leave it active,
//	but undocumented, in case I'm just misunderstanding what it's doing.
//	"  -f, --foreground: Don't daemonize (handy for debugging).\n"
	, argv[0] );
}
