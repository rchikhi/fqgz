/*
 * gzip.c - a file compression and decompression program
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include "prog_util.h"


#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <sys/utime.h>
#else
#  include <sys/time.h>
#  include <unistd.h>
#  include <utime.h>
#endif

struct options {
	bool to_stdout;
	bool force;
	bool keep;
	const tchar *suffix;
    int skip;
    signed long long until;
};

static const tchar *const optstring = T("1::2::3::4::5::6::7::8::9::cdfhknS:s:r:u:V");

static void
show_usage(FILE *fp)
{
	fprintf(fp,
"Usage: %" TS " [-LEVEL] [-cdfhkV] [-S SUF] FILE...\n"
"Compress or decompress the specified FILEs.\n"
"\n"
"Options:\n"
"  -1        fastest (worst) compression\n"
"  -6        medium compression (default)\n"
"  -12       slowest (best) compression\n"
"  -c        write to standard output\n"
"  -d        decompress\n"
"  -f        overwrite existing output files\n"
"  -h        print this help\n"
"  -k        don't delete input files\n"
"  -S SUF    use suffix SUF instead of .gz\n"
"  -s BYTES  skip BYTES of compressed data, then skip 20 blocks, then decompress the rest\n"
"  -u BYTES  stop 20 block after position BYTES in compressed data\n"
"  -V        show version and legal information\n",
	program_invocation_name);
}

static void
show_version(void)
{
	printf(
"gzip compression program v" LIBDEFLATE_VERSION_STRING "\n"
"Copyright 2016 Eric Biggers\n"
"\n"
"This program is free software which may be modified and/or redistributed\n"
"under the terms of the MIT license.  There is NO WARRANTY, to the extent\n"
"permitted by law.  See the COPYING file for details.\n"
	);
}

static const tchar *
get_suffix(const tchar *path, const tchar *suffix)
{
	size_t path_len = tstrlen(path);
	size_t suffix_len = tstrlen(suffix);
	const tchar *p;

	if (path_len <= suffix_len)
		return NULL;
	p = &path[path_len - suffix_len];
	if (tstrxcmp(p, suffix) == 0)
		return p;
	return NULL;
}

static tchar *
append_suffix(const tchar *path, const tchar *suffix)
{
	size_t path_len = tstrlen(path);
	size_t suffix_len = tstrlen(suffix);
	tchar *suffixed_path = new tchar[path_len + suffix_len + 1];

	if (suffixed_path == NULL)
		return NULL;
	tmemcpy(suffixed_path, path, path_len);
	tmemcpy(&suffixed_path[path_len], suffix, suffix_len + 1);
	return suffixed_path;
}

static u32
load_u32_gzip(const byte *p)
{
	return ((u32)p[0] << 0) | ((u32)p[1] << 8) |
		((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int
do_decompress(struct libdeflate_decompressor *decompressor,
	      struct file_stream *in, struct file_stream *out, int skip,
          signed long long until)
{
	const byte *compressed_data = static_cast<const byte*>(in->mmap_mem);
	size_t compressed_size = in->mmap_size;
	byte *uncompressed_data = NULL;
	size_t uncompressed_size;
    size_t actual_uncompressed_size = 0; // in case we decompress less 
	enum libdeflate_result result;
	int ret;

	if (compressed_size < sizeof(u32)) {
	       msg("%" TS ": not in gzip format", in->name);
	       ret = -1;
	       goto out;
	}

	uncompressed_size = load_u32_gzip(&compressed_data[compressed_size - 4]);

	uncompressed_data = new byte[uncompressed_size];
	if (uncompressed_data == NULL) {
		msg("%" TS ": file is probably too large to be processed by this "
		    "program", in->name);
		ret = -1;
		goto out;
	}


	result = libdeflate_gzip_decompress(decompressor,
					    compressed_data,
					    compressed_size,
					    uncompressed_data,
					    uncompressed_size, &actual_uncompressed_size, skip, 
                        until);

	if (result == LIBDEFLATE_INSUFFICIENT_SPACE) {
		msg("%" TS ": file corrupt or too large to be processed by this "
		    "program", in->name);
		ret = -1;
		goto out;
	}

	if (result != LIBDEFLATE_SUCCESS) {
		msg("%" TS ": file corrupt or not in gzip format", in->name);
		ret = -1;
		goto out;
	}

	ret = full_write(out, uncompressed_data, actual_uncompressed_size);
out:
	delete uncompressed_data;
	return ret;
}

static int
stat_file(struct file_stream *in, stat_t *stbuf, bool allow_hard_links)
{
	if (tfstat(in->fd, stbuf) != 0) {
		msg("%" TS ": unable to stat file", in->name);
		return -1;
	}

	if (!S_ISREG(stbuf->st_mode) && !in->is_standard_stream) {
		msg("%" TS " is %s -- skipping",
		    in->name, S_ISDIR(stbuf->st_mode) ? "a directory" :
							"not a regular file");
		return -2;
	}

	if (stbuf->st_nlink > 1 && !allow_hard_links) {
		msg("%" TS " has multiple hard links -- skipping "
		    "(use -f to process anyway)", in->name);
		return -2;
	}

	return 0;
}

static void
restore_mode(struct file_stream *out, const stat_t *stbuf)
{
#ifndef _WIN32
	if (fchmod(out->fd, stbuf->st_mode) != 0)
		msg_errno("%" TS ": unable to preserve mode", out->name);
#endif
}

static void
restore_owner_and_group(struct file_stream *out, const stat_t *stbuf)
{
#ifndef _WIN32
	if (fchown(out->fd, stbuf->st_uid, stbuf->st_gid) != 0) {
		msg_errno("%" TS ": unable to preserve owner and group",
			  out->name);
	}
#endif
}

static void
restore_timestamps(struct file_stream *out, const tchar *newpath,
		   const stat_t *stbuf)
{
	int ret;
#if defined(HAVE_FUTIMENS) && defined(HAVE_STAT_NANOSECOND_PRECISION)
	struct timespec times[2] = {
		stbuf->st_atim, stbuf->st_mtim,
	};
	ret = futimens(out->fd, times);
#elif defined(HAVE_FUTIMES) && defined(HAVE_STAT_NANOSECOND_PRECISION)
	struct timeval times[2] = {
		{ stbuf->st_atim.tv_sec, stbuf->st_atim.tv_nsec / 1000, },
		{ stbuf->st_mtim.tv_sec, stbuf->st_mtim.tv_nsec / 1000, },
	};
	ret = futimes(out->fd, times);
#else
	struct tutimbuf times = {
		stbuf->st_atime, stbuf->st_mtime,
	};
	ret = tutime(newpath, &times);
#endif
	if (ret != 0)
		msg_errno("%" TS ": unable to preserve timestamps", out->name);
}

static void
restore_metadata(struct file_stream *out, const tchar *newpath,
		 const stat_t *stbuf)
{
	restore_mode(out, stbuf);
	restore_owner_and_group(out, stbuf);
	restore_timestamps(out, newpath, stbuf);
}

static int
decompress_file(struct libdeflate_decompressor *decompressor, const tchar *path,
		const struct options *options)
{
	tchar *oldpath = (tchar *)path;
	tchar *newpath = NULL;
	struct file_stream in;
	struct file_stream out;
	stat_t stbuf;
	int ret;
	int ret2;

	if (path != NULL) {
		const tchar *suffix = get_suffix(path, options->suffix);
		if (suffix == NULL) {
			/*
			 * Input file is unsuffixed.  If the file doesn't exist,
			 * then try it suffixed.  Otherwise, if we're not
			 * writing to stdout, skip the file with warning status.
			 * Otherwise, go ahead and try to open the file anyway
			 * (which will very likely fail).
			 */
			if (tstat(path, &stbuf) != 0 && errno == ENOENT) {
				oldpath = append_suffix(path, options->suffix);
				if (oldpath == NULL)
					return -1;
				if (!options->to_stdout)
					newpath = (tchar *)path;
			} else if (!options->to_stdout) {
				msg("\"%" TS "\" does not end with the %" TS " "
				    "suffix -- skipping",
				    path, options->suffix);
				return -2;
			}
		} else if (!options->to_stdout) {
			/*
			 * Input file is suffixed, and we're not writing to
			 * stdout.  Strip the suffix to get the path to the
			 * output file.
			 */
			newpath = new tchar[suffix - oldpath + 1];
			if (newpath == NULL)
				return -1;
			tmemcpy(newpath, oldpath, suffix - oldpath);
			newpath[suffix - oldpath] = '\0';
		}
	}

	ret = xopen_for_read(oldpath, options->force || options->to_stdout,
			     &in);
	if (ret != 0)
		goto out_free_paths;

	if (!options->force && isatty(in.fd)) {
		msg("Refusing to read compressed data from terminal.  "
		    "Use -f to override.\nFor help, use -h.");
		ret = -1;
		goto out_close_in;
	}

	ret = stat_file(&in, &stbuf, options->force || options->keep ||
			oldpath == NULL || newpath == NULL);
	if (ret != 0)
		goto out_close_in;

	ret = xopen_for_write(newpath, options->force, &out);
	if (ret != 0)
		goto out_close_in;

	/* TODO: need a streaming-friendly solution */
	ret = map_file_contents(&in, stbuf.st_size);
	if (ret != 0)
		goto out_close_out;

	ret = do_decompress(decompressor, &in, &out, options->skip, options->until);
	if (ret != 0)
		goto out_close_out;

	if (oldpath != NULL && newpath != NULL)
		restore_metadata(&out, newpath, &stbuf);
	ret = 0;
out_close_out:
	ret2 = xclose(&out);
	if (ret == 0)
		ret = ret2;
	if (ret != 0 && newpath != NULL)
		tunlink(newpath);
out_close_in:
	xclose(&in);
	if (ret == 0 && oldpath != NULL && newpath != NULL && !options->keep)
		tunlink(oldpath);
out_free_paths:
	if (newpath != path)
		delete newpath;
	if (oldpath != path)
		delete oldpath;
	return ret;
}

int
tmain(int argc, tchar *argv[])
{
	tchar *default_file_list[] = { NULL };
	struct options options;
	int opt_char;
	int i;
	int ret;

	_program_invocation_name = get_filename(argv[0]);

	options.to_stdout = false;
	options.force = false;
	options.keep = false;
	options.suffix = T(".gz");
	options.skip = 0;
	options.until = -1;

	while ((opt_char = tgetopt(argc, argv, optstring)) != -1) {
		switch (opt_char) {
		case 'c':
			options.to_stdout = true;
			break;
		case 'f':
			options.force = true;
			break;
		case 'h':
			show_usage(stdout);
			return 0;
		case 'k':
			options.keep = true;
			break;
		case 'n':
			/*
			 * -n means don't save or restore the original filename
			 *  in the gzip header.  Currently this implementation
			 *  already behaves this way by default, so accept the
			 *  option as a no-op.
			 */
			break;
		case 'S':
			options.suffix = toptarg;
			if (options.suffix[0] == T('\0')) {
				msg("invalid suffix");
				return 1;
			}
			break;
		case 's':
			options.skip = atoi(toptarg);
            fprintf(stderr,"skipping %d bytes (experimental)\n",options.skip);
			break;
		case 'u':
			options.until = atoi(toptarg);
            fprintf(stderr,"decoding until 20 blocks after compressed position %lld\n",options.until);
			break;

		case 'V':
			show_version();
			return 0;
		default:
			show_usage(stderr);
			return 1;
		}
	}

	argv += toptind;
	argc -= toptind;

	if (argc == 0) {
		argv = default_file_list;
		argc = ARRAY_LEN(default_file_list);
	} else {
		for (i = 0; i < argc; i++)
			if (argv[i][0] == '-' && argv[i][1] == '\0')
				argv[i] = NULL;
	}

	ret = 0;
    struct libdeflate_decompressor *d;

    d = alloc_decompressor();
    if (d == NULL)
        return 1;

    for (i = 0; i < argc; i++)
        ret |= -decompress_file(d, argv[i], &options);

    libdeflate_free_decompressor(d);

	/*
	 * If ret=0, there were no warnings or errors.  Exit with status 0.
	 * If ret=2, there was at least one warning.  Exit with status 2.
	 * Else, there was at least one error.  Exit with status 1.
	 */
	if (ret != 0 && ret != 2)
		ret = 1;

	return ret;
}
