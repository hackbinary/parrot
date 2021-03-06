/*
 * Copyright (C) 2007-2010, Parrot Foundation.
 */

/*

=head1 NAME

src/platform/generic/stat.c

=head1 DESCRIPTION

File stat stuff

=head2 Functions

=over 4

=cut

*/

#include "parrot/parrot.h"

#include <string.h>

#ifdef PARROT_HAS_HEADER_SYSSTAT
#  include <sys/stat.h>
#endif

/* These can probably be removed if we switch win32 to use the Windows API
 * for stat */

#ifndef S_IFBLK
#  define S_IFBLK 0060000
#endif

#ifndef S_ISBLK
#  define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#endif

#ifndef S_ISCHR
#  define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#endif

#ifndef S_ISDIR
#  define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#ifndef S_ISREG
#  define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

/* HEADERIZER HFILE: none */

/* HEADERIZER BEGIN: static */
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */

static INTVAL stat_common(PARROT_INTERP,
    ARGIN(struct stat *statbuf),
    INTVAL thing,
    int status)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

#define ASSERT_ARGS_stat_common __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(statbuf))
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */
/* HEADERIZER END: static */

/*

=item C<PMC * Parrot_stat_file(PARROT_INTERP, STRING *filename)>

B<Not implemented.>  Returns C<NULL>.

=cut

*/

PARROT_CANNOT_RETURN_NULL
PMC *
Parrot_stat_file(SHIM_INTERP, SHIM(STRING *filename))
{
    return PMCNULL;
}

/*

=item C<PMC * Parrot_stat_info_pmc(PARROT_INTERP, STRING *filename, INTVAL
thing)>

B<Not implemented.>  Returns C<NULL>.

=cut

*/

PARROT_CANNOT_RETURN_NULL
PMC *
Parrot_stat_info_pmc(SHIM_INTERP, SHIM(STRING *filename), SHIM(INTVAL thing))
{
    return PMCNULL;
}

/*

=item C<static INTVAL stat_common(PARROT_INTERP, struct stat *statbuf, INTVAL
thing, int status)>

Stats the file, and returns the information specified by C<thing>. C<thing> can
be one of:

=over 4

=item * C<STAT_EXISTS>

=item * C<STAT_FILESIZE>

=item * C<STAT_ISDIR>

=item * C<STAT_ISREG>

=item * C<STAT_ISDEV>

=item * C<STAT_ACCESSTIME>

=item * C<STAT_MODIFYTIME>

=item * C<STAT_CHANGETIME>

=item * C<STAT_UID>

=item * C<STAT_GID>

=item * C<STAT_PLATFORM_DEV>

=item * C<STAT_PLATFORM_INODE>

=item * C<STAT_PLATFORM_MODE>

=item * C<STAT_PLATFORM_NLINKS>

=item * C<STAT_PLATFORM_DEVTYPE>

=item * C<STAT_PLATFORM_MODE>

=item * C<STAT_PLATFORM_NLINKS>

=item * C<STAT_PLATFORM_DEVTYPE>

=item * C<STAT_PLATFORM_BLOCKSIZE>

=item * C<STAT_PLATFORM_BLOCKS>

=back

C<STAT_CREATETIME> and C<STAT_BACKUPTIME> are not supported and will return C<-1>.

=cut

*/

static INTVAL
stat_common(PARROT_INTERP, ARGIN(struct stat *statbuf), INTVAL thing, int status)
{
    ASSERT_ARGS(stat_common)
    INTVAL result = -1;

    if (thing == STAT_EXISTS)
        return status == 0;

    if (status == -1) {
        const char *err = strerror(errno);
        Parrot_ex_throw_from_c_args(interp, NULL, EXCEPTION_EXTERNAL_ERROR,
            "stat failed: %s", err);
    }

    switch (thing) {
      case STAT_FILESIZE:
        result = statbuf->st_size;
        break;
      case STAT_ISDIR:
        result = S_ISDIR(statbuf->st_mode);
        break;
      case STAT_ISREG:
        result = S_ISREG(statbuf->st_mode);
        break;
      case STAT_ISDEV:
        result = S_ISCHR(statbuf->st_mode) || S_ISBLK(statbuf->st_mode);
        break;
      case STAT_ISLNK:
#ifdef S_ISLNK
        result = S_ISLNK(statbuf->st_mode);
#else
        result = 0;
#endif
        break;
      case STAT_CREATETIME:
        result = -1;
        break;
      case STAT_ACCESSTIME:
        result = statbuf->st_atime;
        break;
      case STAT_MODIFYTIME:
        result = statbuf->st_mtime;
        break;
      case STAT_CHANGETIME:
        result = statbuf->st_ctime;
        break;
      case STAT_BACKUPTIME:
        result = -1;
        break;
      case STAT_UID:
        result = statbuf->st_uid;
        break;
      case STAT_GID:
        result = statbuf->st_gid;
        break;
      case STAT_PLATFORM_DEV:
        result = statbuf->st_dev;
        break;
      case STAT_PLATFORM_INODE:
        result = statbuf->st_ino;
        break;
      case STAT_PLATFORM_MODE:
        result = statbuf->st_mode;
        break;
      case STAT_PLATFORM_NLINKS:
        result = statbuf->st_nlink;
        break;
      case STAT_PLATFORM_DEVTYPE:
        result = statbuf->st_rdev;
        break;
      case STAT_PLATFORM_BLOCKSIZE:
#ifdef PARROT_HAS_BSD_STAT_EXTN
        result = statbuf->st_blksize;
#else
        Parrot_ex_throw_from_c_args(interp, NULL, 1,
                    "STAT_PLATFORM_BLOCKSIZE not supported");
#endif
        break;
      case STAT_PLATFORM_BLOCKS:
#ifdef PARROT_HAS_BSD_STAT_EXTN
        result = statbuf->st_blocks;
#else
        Parrot_ex_throw_from_c_args(interp, NULL, 1,
                    "STAT_PLATFORM_BLOCKS not supported");
#endif
        break;
      default:
        break;
    }

    return result;
}

/*

=item C<INTVAL Parrot_lstat_info_intval(PARROT_INTERP, STRING *file, INTVAL
thing)>

Returns the lstat field given by C<thing> of file C<file>.

=cut

*/

INTVAL
Parrot_lstat_info_intval(PARROT_INTERP, STRING *file, INTVAL thing)
{
    struct stat statbuf;

    /* Get the name of the file as something we can use */
    char * const filename = Parrot_str_to_cstring(interp, file);

    /* Everything needs the result of stat, so just go do it */
#ifdef _WIN32
    const int status = stat(filename, &statbuf);
#else
    const int status = lstat(filename, &statbuf);
#endif
    Parrot_str_free_cstring(filename);
    return stat_common(interp, &statbuf, thing, status);
}

/*

=item C<INTVAL Parrot_stat_info_intval(PARROT_INTERP, STRING *file, INTVAL
thing)>

Returns the stat field given by C<thing> of file C<file>.

=cut

*/

INTVAL
Parrot_stat_info_intval(PARROT_INTERP, STRING *file, INTVAL thing)
{
    struct stat statbuf;

    /* Get the name of the file as something we can use */
    char * const filename = Parrot_str_to_cstring(interp, file);

    /* Everything needs the result of stat, so just go do it */
    const int status = stat(filename, &statbuf);
    Parrot_str_free_cstring(filename);
    return stat_common(interp, &statbuf, thing, status);
}

/*

=item C<INTVAL Parrot_fstat_info_intval(PARROT_INTERP, INTVAL file, INTVAL
thing)>

Returns the fstat field given by C<thing> from file identifier C<file>.

=cut

*/

INTVAL
Parrot_fstat_info_intval(PARROT_INTERP, INTVAL file, INTVAL thing)
{
    struct stat statbuf;
    int status;

    /* Everything needs the result of stat, so just go do it */
    status = fstat(file, &statbuf);
    return stat_common(interp, &statbuf, thing, status);
}

/*

=item C<FLOATVAL Parrot_stat_info_floatval(PARROT_INTERP, STRING *filename,
INTVAL thing)>

Currently returns C<-1.0> and has no side effects.

=cut

*/

FLOATVAL
Parrot_stat_info_floatval(SHIM_INTERP, SHIM(STRING *filename), SHIM(INTVAL thing))
{
    return (FLOATVAL)-1;
}

/*

=item C<STRING * Parrot_stat_info_string(PARROT_INTERP, STRING *filename, INTVAL
thing)>

B<Not implemented.> Returns C<NULL>.

=cut

*/

PARROT_CANNOT_RETURN_NULL
STRING *
Parrot_stat_info_string(SHIM_INTERP, SHIM(STRING *filename), SHIM(INTVAL thing))
{
    return STRINGNULL;
}

/*

=back

=cut

*/

/*
 * Local variables:
 *   c-file-style: "parrot"
 * End:
 * vim: expandtab shiftwidth=4 cinoptions='\:2=2' :
 */
