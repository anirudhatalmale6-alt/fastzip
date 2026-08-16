/*
 * fastzip - a parallel, terminal-friendly ZIP writer for Windows / Linux
 *
 * Speed comes from four places:
 *   1. per-file parallelism      - a work queue spread over every core
 *   2. intra-file parallelism    - large files are split into blocks that are
 *                                  deflated concurrently (pigz-style, using a
 *                                  32 KiB dictionary carried across the block
 *                                  boundary so the ratio barely moves)
 *   3. libdeflate                - ~2x faster than stock zlib at the same level
 *   4. hardware CRC32            - PCLMUL/AVX carry-less multiply, not a table
 *
 * The output is an ordinary ZIP archive. Any tool opens it.
 */

#define __USE_MINGW_ANSI_STDIO 1
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "libdeflate.h"
#include "zlib.h"

#define FZ_VERSION "1.0.0"

/* ------------------------------------------------------------------ */
/* platform: threads, timers, unicode paths, directory walking         */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>

typedef HANDLE          fz_thread;
typedef CRITICAL_SECTION fz_mutex;
typedef CONDITION_VARIABLE fz_cond;

static void fz_mutex_init(fz_mutex *m)   { InitializeCriticalSection(m); }
static void fz_mutex_lock(fz_mutex *m)   { EnterCriticalSection(m); }
static void fz_mutex_unlock(fz_mutex *m) { LeaveCriticalSection(m); }
static void fz_cond_init(fz_cond *c)     { InitializeConditionVariable(c); }
static void fz_cond_wait(fz_cond *c, fz_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void fz_cond_broadcast(fz_cond *c){ WakeAllConditionVariable(c); }

static double fz_now_ms(void)
{
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int fz_ncpu(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (int)si.dwNumberOfProcessors;
}

static wchar_t *fz_widen(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
	if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static char *fz_narrow(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = (char *)malloc((size_t)n);
	if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return s;
}

static FILE *fz_fopen(const char *path, const char *mode)
{
	wchar_t wmode[8], *wp = fz_widen(path);
	FILE *f;
	size_t i = 0;
	if (!wp) return NULL;
	for (i = 0; mode[i] && i < 7; i++) wmode[i] = (wchar_t)mode[i];
	wmode[i] = 0;
	f = _wfopen(wp, wmode);
	free(wp);
	return f;
}
#define fz_fseek _fseeki64
#define fz_ftell _ftelli64

#else /* POSIX */

#include <pthread.h>
#include <unistd.h>
#include <dirent.h>

typedef pthread_t       fz_thread;
typedef pthread_mutex_t fz_mutex;
typedef pthread_cond_t  fz_cond;

static void fz_mutex_init(fz_mutex *m)   { pthread_mutex_init(m, NULL); }
static void fz_mutex_lock(fz_mutex *m)   { pthread_mutex_lock(m); }
static void fz_mutex_unlock(fz_mutex *m) { pthread_mutex_unlock(m); }
static void fz_cond_init(fz_cond *c)     { pthread_cond_init(c, NULL); }
static void fz_cond_wait(fz_cond *c, fz_mutex *m) { pthread_cond_wait(c, m); }
static void fz_cond_broadcast(fz_cond *c){ pthread_cond_broadcast(c); }

static double fz_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int fz_ncpu(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

#define fz_fopen fopen
#define fz_fseek fseeko
#define fz_ftell ftello
#endif

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "fastzip: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) die("out of memory (%llu bytes)", (unsigned long long)n);
	return p;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char *)xmalloc(n);
	memcpy(p, s, n);
	return p;
}

/* ------------------------------------------------------------------ */
/* stat + directory walk                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	uint64_t size;
	int      is_dir;
	time_t   mtime;
} fz_stat_t;

static int fz_stat(const char *path, fz_stat_t *out)
{
#ifdef _WIN32
	struct _stat64 st;
	wchar_t *wp = fz_widen(path);
	int rc;
	if (!wp) return -1;
	rc = _wstat64(wp, &st);
	free(wp);
	if (rc != 0) return -1;
	out->size   = (uint64_t)st.st_size;
	out->is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
	out->mtime  = st.st_mtime;
#else
	struct stat st;
	if (stat(path, &st) != 0) return -1;
	out->size   = (uint64_t)st.st_size;
	out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
	out->mtime  = st.st_mtime;
#endif
	return 0;
}

/* ------------------------------------------------------------------ */
/* entries                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
	uint8_t *buf;      /* compressed bytes for this block           */
	size_t   len;      /* compressed length                          */
	uint32_t rawlen;   /* uncompressed length of this block          */
	uint32_t crc;      /* crc32 of this block's uncompressed bytes   */
	uint64_t acct;     /* bytes charged against the memory budget    */
	int      ready;
} block_t;

typedef struct {
	char    *zipname;   /* path stored inside the archive (UTF-8, '/') */
	char    *fspath;    /* path on disk                                */
	uint64_t usize;
	uint64_t csize;
	uint32_t crc;
	uint16_t method;    /* 0 = store, 8 = deflate                      */
	uint16_t dtime, ddate;
	int      is_dir;
	int      nblocks;
	int      ready_blocks;
	block_t *blocks;
	uint64_t lho;       /* local header offset                         */
	int      zip64;
} entry_t;

typedef struct {
	int      entry;
	int      block;
	uint64_t off;       /* offset of this block inside the file        */
	uint32_t len;       /* raw bytes in this block                     */
} work_t;

typedef struct {
	entry_t *entries;
	int      nentries;
	work_t  *work;
	int      nwork;

	int      next_work;
	int      write_entry;
	uint64_t inflight;
	uint64_t budget;

	fz_mutex mtx;
	fz_cond  cv;

	int      level;
	int      engine;     /* 0 = libdeflate, 1 = zlib */
	uint32_t blocksize;
	struct libdeflate_compressor **comp; /* one per worker */
	int      nthreads;
} ctx_t;

#define ENGINE_LIBDEFLATE 0
#define ENGINE_ZLIB       1

typedef struct { ctx_t *ctx; int id; } worker_arg_t;

/* ------------------------------------------------------------------ */
/* little-endian writers                                               */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void dos_time(time_t t, uint16_t *dtime, uint16_t *ddate)
{
	struct tm tmv;
	struct tm *lt;
#ifdef _WIN32
	lt = localtime(&t);
	if (lt) tmv = *lt;
#else
	lt = localtime_r(&t, &tmv);
#endif
	if (!lt || tmv.tm_year < 80) { /* pre-1980 is not representable in a ZIP */
		*dtime = 0; *ddate = (uint16_t)(1 << 5 | 1);
		return;
	}
	*dtime = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
	*ddate = (uint16_t)(((tmv.tm_year - 80) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

/* ------------------------------------------------------------------ */
/* file list building                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	entry_t *v;
	int      n, cap;
} elist_t;

static entry_t *elist_push(elist_t *l)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 256;
		l->v = (entry_t *)realloc(l->v, (size_t)l->cap * sizeof(entry_t));
		if (!l->v) die("out of memory growing the file list");
	}
	memset(&l->v[l->n], 0, sizeof(entry_t));
	return &l->v[l->n++];
}

static char *join_path(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	char *s = (char *)xmalloc(la + lb + 2);
	memcpy(s, a, la);
	s[la] = '/';
	memcpy(s + la + 1, b, lb + 1);
	return s;
}

static void add_path(elist_t *l, const char *fspath, const char *zipname);

static void add_dir(elist_t *l, const char *fspath, const char *zipname)
{
	entry_t *e = elist_push(l);
	size_t n = strlen(zipname);
	e->zipname = (char *)xmalloc(n + 2);
	memcpy(e->zipname, zipname, n);
	e->zipname[n] = '/';
	e->zipname[n + 1] = 0;
	e->fspath = xstrdup(fspath);
	e->is_dir = 1;
	e->method = 0;
	{
		fz_stat_t st;
		if (fz_stat(fspath, &st) == 0) dos_time(st.mtime, &e->dtime, &e->ddate);
	}

#ifdef _WIN32
	{
		wchar_t *wpat;
		WIN32_FIND_DATAW fd;
		HANDLE h;
		char *pat = (char *)xmalloc(strlen(fspath) + 3);
		sprintf(pat, "%s/*", fspath);
		wpat = fz_widen(pat);
		free(pat);
		h = FindFirstFileW(wpat, &fd);
		free(wpat);
		if (h == INVALID_HANDLE_VALUE) return;
		do {
			char *name, *cf, *cz;
			if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
			name = fz_narrow(fd.cFileName);
			if (!name) continue;
			cf = join_path(fspath, name);
			cz = join_path(zipname, name);
			add_path(l, cf, cz);
			free(cf); free(cz); free(name);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
#else
	{
		DIR *d = opendir(fspath);
		struct dirent *de;
		if (!d) return;
		while ((de = readdir(d)) != NULL) {
			char *cf, *cz;
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
			cf = join_path(fspath, de->d_name);
			cz = join_path(zipname, de->d_name);
			add_path(l, cf, cz);
			free(cf); free(cz);
		}
		closedir(d);
	}
#endif
}

static void add_path(elist_t *l, const char *fspath, const char *zipname)
{
	fz_stat_t st;
	if (fz_stat(fspath, &st) != 0) {
		fprintf(stderr, "fastzip: warning: cannot stat %s (skipped)\n", fspath);
		return;
	}
	if (st.is_dir) {
		add_dir(l, fspath, zipname);
	} else {
		entry_t *e = elist_push(l);
		e->zipname = xstrdup(zipname);
		e->fspath  = xstrdup(fspath);
		e->usize   = st.size;
		e->method  = 8;
		dos_time(st.mtime, &e->dtime, &e->ddate);
	}
}

/* strip drive letters / leading separators so the archive holds relative paths */
static const char *basename_of(const char *p)
{
	const char *b = p, *s;
	for (s = p; *s; s++)
		if (*s == '/' || *s == '\\') b = s + 1;
	if (*b == 0 && b != p) { /* trailing separator: back up one component */
		const char *e = b - 1, *b2 = p;
		for (s = p; s < e; s++)
			if (*s == '/' || *s == '\\') b2 = s + 1;
		return b2;
	}
	return b;
}

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(((const entry_t *)a)->zipname, ((const entry_t *)b)->zipname);
}

/* ------------------------------------------------------------------ */
/* compression workers                                                 */
/* ------------------------------------------------------------------ */

/* whole-file path: read it, crc it, deflate it in one shot */
static void compress_whole(ctx_t *c, int wid, entry_t *e, block_t *b, uint32_t len)
{
	uint8_t *raw = NULL, *out = NULL;
	size_t outcap, outlen = 0;
	FILE *f;

	raw = (uint8_t *)xmalloc(len ? len : 1);
	f = fz_fopen(e->fspath, "rb");
	if (!f) die("cannot open %s: %s", e->fspath, strerror(errno));
	if (len && fread(raw, 1, len, f) != len) die("short read on %s", e->fspath);
	fclose(f);

	b->crc = libdeflate_crc32(0, raw, len);
	b->rawlen = len;

	if (c->level == 0 || len == 0) {
		b->buf = raw; b->len = len; e->method = 0;
		return;
	}

	outcap = len + len / 8 + 128;
	out = (uint8_t *)xmalloc(outcap);

	if (c->engine == ENGINE_LIBDEFLATE) {
		outlen = libdeflate_deflate_compress(c->comp[wid], raw, len, out, outcap);
	} else {
		z_stream zs;
		memset(&zs, 0, sizeof(zs));
		if (deflateInit2(&zs, c->level > 9 ? 9 : c->level, Z_DEFLATED, -15, 9,
		                 Z_DEFAULT_STRATEGY) != Z_OK)
			die("deflateInit2 failed");
		zs.next_in = raw; zs.avail_in = len;
		zs.next_out = out; zs.avail_out = (uInt)outcap;
		if (deflate(&zs, Z_FINISH) == Z_STREAM_END) outlen = outcap - zs.avail_out;
		else outlen = 0; /* did not fit -> store */
		deflateEnd(&zs);
	}

	if (outlen == 0 || outlen >= (size_t)len) {
		/* deflate did not help: store the file verbatim */
		free(out);
		b->buf = raw; b->len = len; e->method = 0;
	} else {
		free(raw);
		b->buf = out; b->len = outlen; e->method = 8;
	}
}

/*
 * block path for large files. Each block is deflated independently but is
 * primed with the previous 32 KiB of input as a dictionary, so the ratio stays
 * within a fraction of a percent of a single-stream deflate. Every block ends
 * on a byte boundary (Z_SYNC_FLUSH), so the blocks concatenate into one valid
 * deflate stream; the final block closes it with Z_FINISH.
 */
static void compress_block(ctx_t *c, entry_t *e, block_t *b, uint64_t off, uint32_t len,
                           int is_last)
{
	uint8_t *raw, *dict = NULL, *out;
	uint32_t dictlen = 0;
	size_t outcap;
	z_stream zs;
	FILE *f;
	int rc;

	if (off > 0) dictlen = off >= 32768 ? 32768 : (uint32_t)off;

	raw  = (uint8_t *)xmalloc(len);
	if (dictlen) dict = (uint8_t *)xmalloc(dictlen);

	f = fz_fopen(e->fspath, "rb");
	if (!f) die("cannot open %s: %s", e->fspath, strerror(errno));
	if (dictlen) {
		if (fz_fseek(f, (int64_t)(off - dictlen), SEEK_SET) != 0) die("seek failed on %s", e->fspath);
		if (fread(dict, 1, dictlen, f) != dictlen) die("short dictionary read on %s", e->fspath);
	} else {
		if (fz_fseek(f, (int64_t)off, SEEK_SET) != 0) die("seek failed on %s", e->fspath);
	}
	if (fread(raw, 1, len, f) != len) die("short read on %s", e->fspath);
	fclose(f);

	b->crc = libdeflate_crc32(0, raw, len);
	b->rawlen = len;

	outcap = len + len / 8 + 256;
	out = (uint8_t *)xmalloc(outcap);

	memset(&zs, 0, sizeof(zs));
	if (deflateInit2(&zs, c->level > 9 ? 9 : c->level, Z_DEFLATED, -15, 9,
	                 Z_DEFAULT_STRATEGY) != Z_OK)
		die("deflateInit2 failed");
	if (dictlen) deflateSetDictionary(&zs, dict, dictlen);

	zs.next_in = raw; zs.avail_in = len;
	zs.next_out = out; zs.avail_out = (uInt)outcap;
	rc = deflate(&zs, is_last ? Z_FINISH : Z_SYNC_FLUSH);
	if (zs.avail_in != 0 || (is_last && rc != Z_STREAM_END))
		die("deflate block overflowed its output buffer");
	b->len = outcap - zs.avail_out;
	deflateEnd(&zs);

	b->buf = out;
	free(raw);
	free(dict);
}

static void worker_body(ctx_t *c, int wid)
{
	for (;;) {
		work_t w;
		entry_t *e;
		block_t *b;
		int idx;

		fz_mutex_lock(&c->mtx);
		idx = c->next_work < c->nwork ? c->next_work++ : -1;
		fz_mutex_unlock(&c->mtx);
		if (idx < 0) return;

		w = c->work[idx];
		e = &c->entries[w.entry];
		b = &e->blocks[w.block];

		/* keep memory bounded: never run far ahead of the writer */
		fz_mutex_lock(&c->mtx);
		while (c->inflight + w.len > c->budget && w.entry > c->write_entry)
			fz_cond_wait(&c->cv, &c->mtx);
		c->inflight += w.len;
		fz_mutex_unlock(&c->mtx);
		b->acct = w.len;

		if (e->nblocks == 1)
			compress_whole(c, wid, e, b, w.len);
		else
			compress_block(c, e, b, w.off, w.len, w.block == e->nblocks - 1);

		fz_mutex_lock(&c->mtx);
		b->ready = 1;
		e->ready_blocks++;
		fz_cond_broadcast(&c->cv);
		fz_mutex_unlock(&c->mtx);
	}
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(LPVOID p)
{
	worker_arg_t *a = (worker_arg_t *)p;
	worker_body(a->ctx, a->id);
	return 0;
}
#else
static void *worker_entry(void *p)
{
	worker_arg_t *a = (worker_arg_t *)p;
	worker_body(a->ctx, a->id);
	return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/* verification: re-inflate an entry and check its CRC                 */
/* ------------------------------------------------------------------ */

static int verify_entry(entry_t *e)
{
	z_stream zs;
	uint8_t outbuf[65536];
	uint32_t crc = 0;
	uint64_t total = 0;
	int i, rc = Z_OK;

	if (e->method == 0) {
		for (i = 0; i < e->nblocks; i++) {
			crc = libdeflate_crc32(crc, e->blocks[i].buf, e->blocks[i].len);
			total += e->blocks[i].len;
		}
		return (crc == e->crc && total == e->usize) ? 0 : -1;
	}

	memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, -15) != Z_OK) return -1;
	for (i = 0; i < e->nblocks; i++) {
		zs.next_in  = e->blocks[i].buf;
		zs.avail_in = (uInt)e->blocks[i].len;
		do {
			zs.next_out  = outbuf;
			zs.avail_out = sizeof(outbuf);
			rc = inflate(&zs, Z_NO_FLUSH);
			if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
				inflateEnd(&zs);
				return -1;
			}
			{
				size_t got = sizeof(outbuf) - zs.avail_out;
				crc = libdeflate_crc32(crc, outbuf, got);
				total += got;
			}
		} while (zs.avail_out == 0);
	}
	inflateEnd(&zs);
	return (crc == e->crc && total == e->usize) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* archive writer                                                      */
/* ------------------------------------------------------------------ */

#define Z64_LIMIT 0xFFFFFFFFULL

static void write_local_header(FILE *out, entry_t *e)
{
	uint8_t h[30];
	size_t nlen = strlen(e->zipname);
	int need64 = (e->usize >= Z64_LIMIT || e->csize >= Z64_LIMIT);
	uint16_t xlen = need64 ? 20 : 0;

	e->zip64 = need64;

	put32(h + 0,  0x04034b50);
	put16(h + 4,  need64 ? 45 : 20);
	put16(h + 6,  0x0800);              /* names are UTF-8 */
	put16(h + 8,  e->method);
	put16(h + 10, e->dtime);
	put16(h + 12, e->ddate);
	put32(h + 14, e->crc);
	put32(h + 18, need64 ? 0xFFFFFFFFu : (uint32_t)e->csize);
	put32(h + 22, need64 ? 0xFFFFFFFFu : (uint32_t)e->usize);
	put16(h + 26, (uint16_t)nlen);
	put16(h + 28, xlen);
	fwrite(h, 1, 30, out);
	fwrite(e->zipname, 1, nlen, out);

	if (need64) {
		uint8_t x[20];
		put16(x + 0, 0x0001);
		put16(x + 2, 16);
		put64(x + 4, e->usize);
		put64(x + 12, e->csize);
		fwrite(x, 1, 20, out);
	}
}

static void write_central(FILE *out, entry_t *e)
{
	uint8_t h[46], x[32];
	size_t nlen = strlen(e->zipname);
	int xn = 0;
	int big_u = e->usize >= Z64_LIMIT;
	int big_c = e->csize >= Z64_LIMIT;
	int big_o = e->lho   >= Z64_LIMIT;
	int need64 = big_u || big_c || big_o;

	if (need64) {
		int p = 4;
		if (big_u) { put64(x + p, e->usize); p += 8; }
		if (big_c) { put64(x + p, e->csize); p += 8; }
		if (big_o) { put64(x + p, e->lho);   p += 8; }
		put16(x + 0, 0x0001);
		put16(x + 2, (uint16_t)(p - 4));
		xn = p;
	}

	put32(h + 0,  0x02014b50);
	put16(h + 4,  need64 ? 45 : 20);    /* version made by  */
	put16(h + 6,  need64 ? 45 : 20);    /* version needed   */
	put16(h + 8,  0x0800);
	put16(h + 10, e->method);
	put16(h + 12, e->dtime);
	put16(h + 14, e->ddate);
	put32(h + 16, e->crc);
	put32(h + 20, big_c ? 0xFFFFFFFFu : (uint32_t)e->csize);
	put32(h + 24, big_u ? 0xFFFFFFFFu : (uint32_t)e->usize);
	put16(h + 28, (uint16_t)nlen);
	put16(h + 30, (uint16_t)xn);
	put16(h + 32, 0);                   /* comment length   */
	put16(h + 34, 0);                   /* disk number      */
	put16(h + 36, 0);                   /* internal attrs   */
	put32(h + 38, e->is_dir ? 0x10 : 0x20); /* DOS attrs    */
	put32(h + 42, big_o ? 0xFFFFFFFFu : (uint32_t)e->lho);
	fwrite(h, 1, 46, out);
	fwrite(e->zipname, 1, nlen, out);
	if (xn) fwrite(x, 1, (size_t)xn, out);
}

static void write_end(FILE *out, ctx_t *c, uint64_t cd_off, uint64_t cd_size)
{
	uint8_t h[22];
	int need64 = (c->nentries > 0xFFFF || cd_off >= Z64_LIMIT || cd_size >= Z64_LIMIT);

	if (need64) {
		uint8_t z[56], l[20];
		uint64_t z64off = (uint64_t)fz_ftell(out);
		memset(z, 0, sizeof(z));
		put32(z + 0,  0x06064b50);
		put64(z + 4,  44);              /* size of this record - 12 */
		put16(z + 12, 45);
		put16(z + 14, 45);
		put32(z + 16, 0);
		put32(z + 20, 0);
		put64(z + 24, (uint64_t)c->nentries);
		put64(z + 32, (uint64_t)c->nentries);
		put64(z + 40, cd_size);
		put64(z + 48, cd_off);
		fwrite(z, 1, 56, out);

		put32(l + 0,  0x07064b50);
		put32(l + 4,  0);
		put64(l + 8,  z64off);
		put32(l + 16, 1);
		fwrite(l, 1, 20, out);
	}

	put32(h + 0,  0x06054b50);
	put16(h + 4,  0);
	put16(h + 6,  0);
	put16(h + 8,  need64 ? 0xFFFF : (uint16_t)c->nentries);
	put16(h + 10, need64 ? 0xFFFF : (uint16_t)c->nentries);
	put32(h + 12, cd_size >= Z64_LIMIT ? 0xFFFFFFFFu : (uint32_t)cd_size);
	put32(h + 16, cd_off  >= Z64_LIMIT ? 0xFFFFFFFFu : (uint32_t)cd_off);
	put16(h + 20, 0);
	fwrite(h, 1, 22, out);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf(
"fastzip " FZ_VERSION " - parallel ZIP writer\n"
"\n"
"usage: fastzip [options] <archive.zip> <file|directory> [more ...]\n"
"\n"
"options:\n"
"  -t, --threads N     worker threads (default: one per logical core)\n"
"  -l, --level N       compression level 1..12, 0 = store (default 6)\n"
"      --engine E      libdeflate (default) or zlib\n"
"      --block MB      block size for splitting large files (default 1, 0 = off)\n"
"      --baseline      emulate a conventional tool: 1 thread, zlib, no blocks\n"
"      --verify        re-inflate every entry and check its CRC before writing\n"
"      --memory MB     cap on in-flight compressed data (default 1024)\n"
"      --json          print the run summary as one line of JSON\n"
"  -q, --quiet         only print errors\n"
"  -h, --help          this text\n");
}

int main(int argc, char **argv)
{
	ctx_t c;
	elist_t list;
	const char *outpath = NULL;
	char **inputs;
	int ninputs = 0, i, j, quiet = 0, json = 0, verify = 0;
	int threads = 0, level = 6, baseline = 0;
	uint64_t memcap = 1024ull * 1024 * 1024;
	uint32_t blockmb = 1;
	FILE *out;
	double t0, t1;
	uint64_t total_in = 0, total_out = 0, cd_off, cd_size;
	worker_arg_t *wargs;
	fz_thread *tids;
	int nfiles = 0;

	memset(&c, 0, sizeof(c));
	memset(&list, 0, sizeof(list));
	c.engine = ENGINE_LIBDEFLATE;
	inputs = (char **)xmalloc(sizeof(char *) * (size_t)(argc + 1));

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
		else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) quiet = 1;
		else if (!strcmp(a, "--json")) json = 1;
		else if (!strcmp(a, "--verify")) verify = 1;
		else if (!strcmp(a, "--baseline")) baseline = 1;
		else if ((!strcmp(a, "-t") || !strcmp(a, "--threads")) && i + 1 < argc) threads = atoi(argv[++i]);
		else if ((!strcmp(a, "-l") || !strcmp(a, "--level")) && i + 1 < argc) level = atoi(argv[++i]);
		else if (!strcmp(a, "--engine") && i + 1 < argc) {
			const char *e = argv[++i];
			if (!strcmp(e, "zlib")) c.engine = ENGINE_ZLIB;
			else if (!strcmp(e, "libdeflate")) c.engine = ENGINE_LIBDEFLATE;
			else die("unknown engine '%s' (use libdeflate or zlib)", e);
		}
		else if (!strcmp(a, "--block") && i + 1 < argc) blockmb = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(a, "--memory") && i + 1 < argc) memcap = (uint64_t)atoi(argv[++i]) * 1024 * 1024;
		else if (a[0] == '-' && a[1]) die("unknown option '%s' (try --help)", a);
		else if (!outpath) outpath = a;
		else inputs[ninputs++] = argv[i];
	}

	if (!outpath || ninputs == 0) { usage(); return 2; }

	if (baseline) { threads = 1; c.engine = ENGINE_ZLIB; blockmb = 0; }
	if (threads <= 0) threads = fz_ncpu();
	if (threads > 256) threads = 256;
	if (level < 0) level = 0;
	if (level > 12) level = 12;
	if (memcap < 32ull * 1024 * 1024) memcap = 32ull * 1024 * 1024;

	c.level     = level;
	c.nthreads  = threads;
	c.budget    = memcap;
	c.blocksize = blockmb ? blockmb * 1024u * 1024u : 0;

	/* ---- build the file list ---------------------------------- */
	for (i = 0; i < ninputs; i++) {
		char *p = xstrdup(inputs[i]);
		size_t n = strlen(p);
		while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = 0;
		add_path(&list, p, basename_of(p));
		free(p);
	}
	if (list.n == 0) die("nothing to archive");

	qsort(list.v, (size_t)list.n, sizeof(entry_t), cmp_entry);
	c.entries  = list.v;
	c.nentries = list.n;

	/* ---- build the work list ---------------------------------- */
	{
		int cap = 0;
		for (i = 0; i < c.nentries; i++) {
			entry_t *e = &c.entries[i];
			if (e->is_dir) { e->nblocks = 0; continue; }
			nfiles++;
			total_in += e->usize;
			if (c.blocksize && threads > 1 && e->usize > (uint64_t)c.blocksize * 2 && level > 0) {
				e->nblocks = (int)((e->usize + c.blocksize - 1) / c.blocksize);
				e->method  = 8;
			} else {
				e->nblocks = 1;
			}
			cap += e->nblocks;
		}
		c.work = (work_t *)xmalloc(sizeof(work_t) * (size_t)(cap ? cap : 1));
		c.nwork = 0;
		for (i = 0; i < c.nentries; i++) {
			entry_t *e = &c.entries[i];
			if (e->nblocks == 0) continue;
			e->blocks = (block_t *)xmalloc(sizeof(block_t) * (size_t)e->nblocks);
			memset(e->blocks, 0, sizeof(block_t) * (size_t)e->nblocks);
			for (j = 0; j < e->nblocks; j++) {
				uint64_t off = (uint64_t)j * c.blocksize;
				uint64_t len = e->nblocks == 1 ? e->usize : e->usize - off;
				if (e->nblocks > 1 && len > c.blocksize) len = c.blocksize;
				c.work[c.nwork].entry = i;
				c.work[c.nwork].block = j;
				c.work[c.nwork].off   = e->nblocks == 1 ? 0 : off;
				c.work[c.nwork].len   = (uint32_t)len;
				c.nwork++;
			}
		}
	}

	out = fz_fopen(outpath, "wb");
	if (!out) die("cannot create %s: %s", outpath, strerror(errno));
	setvbuf(out, NULL, _IOFBF, 1 << 20);

	fz_mutex_init(&c.mtx);
	fz_cond_init(&c.cv);

	c.comp = (struct libdeflate_compressor **)xmalloc(sizeof(void *) * (size_t)threads);
	for (i = 0; i < threads; i++) {
		c.comp[i] = libdeflate_alloc_compressor(level > 0 ? level : 1);
		if (!c.comp[i]) die("cannot allocate a libdeflate compressor");
	}

	t0 = fz_now_ms();

	wargs = (worker_arg_t *)xmalloc(sizeof(worker_arg_t) * (size_t)threads);
	tids  = (fz_thread *)xmalloc(sizeof(fz_thread) * (size_t)threads);
	for (i = 0; i < threads; i++) {
		wargs[i].ctx = &c;
		wargs[i].id  = i;
#ifdef _WIN32
		tids[i] = CreateThread(NULL, 0, worker_entry, &wargs[i], 0, NULL);
		if (!tids[i]) die("CreateThread failed");
#else
		if (pthread_create(&tids[i], NULL, worker_entry, &wargs[i]) != 0)
			die("pthread_create failed");
#endif
	}

	/* ---- writer: entries go out strictly in order -------------- */
	for (i = 0; i < c.nentries; i++) {
		entry_t *e = &c.entries[i];
		uint64_t freed = 0;

		if (!e->is_dir) {
			fz_mutex_lock(&c.mtx);
			while (e->ready_blocks < e->nblocks) fz_cond_wait(&c.cv, &c.mtx);
			fz_mutex_unlock(&c.mtx);

			e->csize = 0;
			e->crc   = 0;
			for (j = 0; j < e->nblocks; j++) {
				e->csize += e->blocks[j].len;
				e->crc = (j == 0) ? e->blocks[j].crc
				                  : (uint32_t)crc32_combine(e->crc, e->blocks[j].crc,
				                                            (z_off_t)e->blocks[j].rawlen);
			}
			if (e->method == 0) e->csize = e->usize;

			if (verify && verify_entry(e) != 0)
				die("verification failed for %s", e->zipname);
		}

		e->lho = (uint64_t)fz_ftell(out);
		write_local_header(out, e);
		for (j = 0; j < e->nblocks; j++) {
			if (e->blocks[j].len && fwrite(e->blocks[j].buf, 1, e->blocks[j].len, out) != e->blocks[j].len)
				die("write failed on %s (disk full?)", outpath);
			total_out += e->blocks[j].len;
			free(e->blocks[j].buf);
			e->blocks[j].buf = NULL;
			freed += e->blocks[j].acct;
		}

		fz_mutex_lock(&c.mtx);
		c.inflight -= freed;
		c.write_entry = i + 1;
		fz_cond_broadcast(&c.cv);
		fz_mutex_unlock(&c.mtx);
	}

	for (i = 0; i < threads; i++) {
#ifdef _WIN32
		WaitForSingleObject(tids[i], INFINITE);
		CloseHandle(tids[i]);
#else
		pthread_join(tids[i], NULL);
#endif
	}

	cd_off = (uint64_t)fz_ftell(out);
	for (i = 0; i < c.nentries; i++) write_central(out, &c.entries[i]);
	cd_size = (uint64_t)fz_ftell(out) - cd_off;
	write_end(out, &c, cd_off, cd_size);

	if (fflush(out) != 0 || fclose(out) != 0) die("failed to close %s", outpath);

	t1 = fz_now_ms();

	for (i = 0; i < threads; i++) libdeflate_free_compressor(c.comp[i]);

	{
		double ms = t1 - t0;
		double mib = (double)total_in / (1024.0 * 1024.0);
		double ratio = total_in ? 100.0 * (double)total_out / (double)total_in : 0.0;
		const char *eng = baseline ? "zlib (baseline)"
		                           : (c.engine == ENGINE_ZLIB ? "zlib" : "libdeflate");
		if (json) {
			printf("{\"tool\":\"fastzip\",\"version\":\"" FZ_VERSION "\",\"engine\":\"%s\","
			       "\"threads\":%d,\"level\":%d,\"files\":%d,\"bytes_in\":%llu,"
			       "\"bytes_out\":%llu,\"ratio_pct\":%.2f,\"elapsed_ms\":%.1f,"
			       "\"throughput_mib_s\":%.1f}\n",
			       eng, threads, level, nfiles,
			       (unsigned long long)total_in, (unsigned long long)total_out,
			       ratio, ms, ms > 0 ? mib / (ms / 1000.0) : 0.0);
		} else if (!quiet) {
			printf("fastzip " FZ_VERSION "  engine=%s threads=%d level=%d\n", eng, threads, level);
			printf("  %d files, %.2f MiB in -> %.2f MiB out (%.1f%%)\n",
			       nfiles, mib, (double)total_out / (1024.0 * 1024.0), ratio);
			printf("  elapsed: %.1f ms   throughput: %.1f MiB/s\n",
			       ms, ms > 0 ? mib / (ms / 1000.0) : 0.0);
			if (verify) printf("  verified: every entry re-inflated and CRC-matched\n");
		}
	}
	return 0;
}
