// Copyright 2021 Mark Kubacki.
// Use of this source code is governed by the BSD-3 license.

// Assembles the destination file from a number of source files
// the most efficient way. All are expected to be on the same file system.

#define _GNU_SOURCE

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#include <ctype.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif
#ifndef EXIT_INVALIDARGUMENT
static const int EXIT_INVALIDARGUMENT = 2;
#endif

static int has_copy_file_range = 1; // since Linux 5.3
static int has_ficlone = 1;         // 4.5, for BTRFS 2.6.35
static int has_otmpfile = 1;        // 3.11

static __u32
as_version(uint major, uint minor, uint patch) {
  return major<<24 | minor <<16 | patch;
}

static __u32
version_from(const char *p) {
  int ver[3] = {0}; // major, minor, patch, …
  uint i = 0;
  while (*p && i < (sizeof(ver)/sizeof(ver[0]))) {
    if (isdigit(*p)) {
      ver[i] *= 10;
      ver[i] += ((*p) - '0');
    } else {
      if ((*p) != '.') {
        break;
      }
      i++;
    }
    p++;
  }
  return as_version(ver[0], ver[1], ver[2]);
}

static void
init_features() {
  struct utsname lnx = {0};
  errno = 0;
  if (uname(&lnx) != 0) {
    perror("uname");
    exit(EXIT_FAILURE);
  }

  __u32 kv = version_from(lnx.release);
  // copy_file_range from v4.5 has received changes in 5.3
  has_copy_file_range = (kv >= as_version(5, 3, 0)) ? 1 : 0;
  // Backports of O_TMPFILE without backported fs can result in corruptions.
  has_otmpfile =        (kv >= as_version(3, 11, 0)) ? 1 : 0;

#ifdef __GLIBC__
  // Avoid the wrapper, we have our FICLONERANGE and sendfile for that.
  // Even if Linux is the right version, syscall filters could be the
  // cause for ENOSYS and triggering glibc's fallback.
  if (version_from(gnu_get_libc_version()) < as_version(2, 30, 0)) {
    has_copy_file_range = 0;
  }
#endif
}

static char*
directory_from_filename(const char *fname) {
  char *fullpath = canonicalize_file_name(fname);
  if (fullpath == NULL) {
    perror("realpath");
    exit(EXIT_FAILURE);
  }
  char *directory = dirname(fullpath);
  if (directory == NULL) {
    perror("dirname");
    exit(EXIT_FAILURE);
  };
  return directory;
}

int
main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <source...> <destination>\n", argv[0]);
    exit(EXIT_INVALIDARGUMENT);
  }

  init_features();

  int fd_out    = -1;   // Filedesc of the new resulting file.
  char *outdir  = NULL; // If nonnull, O_TMPFILE is being used in this dir.
  char *outfile = argv[argc-1]; // User-provided name of the resulting file.
  if (has_otmpfile) {
    // Per program description, sources and destination shall reside
    // on the same filesystem. Use the first source because the likes of
    // 'realpath' and 'canonicalize_file_name' expect an existing file.
    outdir = directory_from_filename(argv[1]);
    fd_out = open(outdir, O_TMPFILE|O_WRONLY|O_CLOEXEC, 0644);
  }
  if (fd_out == -1) {
    fd_out = open(outfile, O_CREAT|O_WRONLY|O_TRUNC, 0644);
    if (has_otmpfile) {
      free(outdir);
      outdir = NULL;
    }
  }
  if (fd_out == -1) {
    perror("open(output)");
    exit(EXIT_FAILURE);
  }
  size_t fd_size = 0; // Size of the output file, and pos to write after.

  // Open the source files in advance so they're available even if deleted.
  // If file descriptor 0 were available, it would've been claimed by fd_out.
  int *arr_infd = alloca((argc-2+1) * sizeof(int)); // Array of input filedesc.
  for (int i = 1; i < (argc-1); i++) { // Skip argv[last]: It's the output.
    int f = open(argv[i], O_RDONLY|O_NOCTTY|O_CLOEXEC);
    if (f == -1) {
      fprintf(stderr, "Cannot open: %s\n", argv[i]);
      exit(EXIT_FAILURE);
    }
    arr_infd[i-1] = f; // Precedes argv by one because argv[0] is no source.
  }
  arr_infd[argc-2] = '\0'; // To terminate the loop reading this list.

  struct statfs fsinfo = {0};
  if (fstatfs(fd_out, &fsinfo) == -1) {
    perror("fstatfs");
    exit(EXIT_FAILURE);
  }
  size_t fs_blocksize = fsinfo.f_bsize; // Quantum for blockwise transfers.

  // Append to fd_out from every input file.
  while (*arr_infd) {
    int fd_in = *arr_infd;
    struct stat finfo = {0};
    if (fstat(fd_in, &finfo) == -1) {
      perror("fstat");
      exit(EXIT_FAILURE);
    }
    size_t remain = finfo.st_size; // Amount yet to read from the input file.
    ssize_t n = 0; // Bytes transferred by the last operation.

#if defined(_GNU_SOURCE) && defined(__GLIBC_PREREQ) && __GLIBC_PREREQ(2,27)
    if (has_copy_file_range) {
      static const size_t max_count = 1<<30;
      do {
        ssize_t count = max_count;
        if (remain < max_count) {
          count = remain;
        }
        errno = 0;
        n = copy_file_range(fd_in, NULL, fd_out, NULL, count, 0);
        if (n == -1) {
          if (errno == EINTR) {
            continue;
          }
          if (errno == ENOSYS) {
            has_copy_file_range = 0;
            break;
          }
          perror("copy_file_range");
          exit(EXIT_FAILURE);
        }
        remain -= n;
        fd_size += n;
      } while (remain > 0 && n > 0);
    }
#endif

    // ficlone is a fallback, and unless something very unlikely and strange
    // happened, will be called with the input file from 0 to its end here.
    if (has_ficlone &&
        remain > fs_blocksize && (fd_size%fs_blocksize == 0)) {
      struct file_clone_range cfg = {
        .src_fd      = fd_in,
        .src_offset  = 0,
        .src_length  = remain,
        .dest_offset = fd_size,
      };

      // ficlone will get the entire file in one go.
      do {
        errno = 0;
        n = ioctl(fd_out, FICLONERANGE, &cfg);
      } while (n == -1 && errno == EINTR);

      if (n == -1) {
        if (errno == ENOSYS || errno == EBADF || errno == EOPNOTSUPP) {
          has_ficlone = 0;
        } else {
          perror("ficlone");
          exit(EXIT_FAILURE);
        }
      } else {
        n = lseek(fd_out, 0, SEEK_END) - fd_size;
        if (errno != 0) {
          perror("lseek");
          exit(EXIT_FAILURE);
        }
        remain -= n;
        fd_size += n;
      }
    }

    if (remain > 0) {
      static const size_t max_count = 0x7ffff000;
      do {
        size_t count = max_count;
        if (remain < max_count) {
          count = remain;
        }
        errno = 0;
        n = sendfile(fd_out, fd_in, NULL, count);
        if (n == -1) {
          if (errno == EINTR) {
            continue;
          }
          perror("sendfile");
          exit(EXIT_FAILURE);
        }
        remain -= n;
        fd_size += n;
      } while (remain > 0 && n > 0);
    }

    close(fd_in);
    *arr_infd = '\0';
    arr_infd++;

    if (remain > 0) {
      fprintf(stderr, "Failed to read the input file completely.\n");
      exit(EXIT_FAILURE);
    }
  }

  // Give the O_TMPFILE a name. outdir==NULL indicates it's no O_TMPFILE.
  if (outdir != NULL) {
    // Before closing it, name the output file else it gets discarded.
    int n = linkat(fd_out, "", AT_FDCWD, outfile, AT_EMPTY_PATH);
    if (n == -1 && errno == EEXIST) {
      if (remove(outfile) == -1) {
        perror("unlink(output)");
        exit(EXIT_FAILURE);
      }
      n = linkat(fd_out, "", AT_FDCWD, outfile, AT_EMPTY_PATH);
    }
    if (n == -1 && errno != ENOENT) {
      perror("linkat(output)");
      exit(EXIT_FAILURE);
    }

    // Without CAP_DAC_READ_SEARCH, unprivileged users don't have,
    // above linkat will fail. That's the workaround:
    if (n == -1) {
      char *buf = alloca(sizeof("/proc/self/fd/")+9);
      sprintf(buf, "/proc/self/fd/%d", fd_out);
      n = linkat(fd_out, buf, AT_FDCWD, outfile, AT_SYMLINK_FOLLOW);
      if (n == -1 && errno == EEXIST) {
        if (remove(outfile) == -1) {
          perror("unlink(output)");
          exit(EXIT_FAILURE);
        }
        n = linkat(fd_out, buf, AT_FDCWD, outfile, AT_SYMLINK_FOLLOW);
      }
      if (n == -1) {
        perror("linkat(output)");
        exit(EXIT_FAILURE);
      }
    }
    free(outdir);
    outdir = NULL;
  }

  close(fd_out);
  exit(EXIT_SUCCESS);
}
