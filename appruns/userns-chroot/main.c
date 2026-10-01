#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Exit status to use when launching an AppImage fails.
 * For applications that assign meanings to exit status codes (e.g. rsync),
 * we avoid "cluttering" pre-defined exit status codes by using 127 which
 * is known to alias an application exit status and also known as launcher
 * error, see SYSTEM(3POSIX).
 */
#define EXIT_EXECERROR 127

static const char* argv0;
static const char* appdir;
static const char* mountroot;

static void die_if(bool cond, const char* fmt, ...)
{
	if (cond) {
		fprintf(stderr, "%s: ", argv0);
		va_list args;
		va_start(args, fmt);
		vfprintf(stderr, fmt, args);
		va_end(args);
		fprintf(stderr, ": %s\n", strerror(errno));
		exit(EXIT_EXECERROR);
	}
}

static void warn(const char* fmt, ...)
{
	int saved_errno = errno;
	fprintf(stderr, "%s: ", argv0);
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, ": %s\n", strerror(saved_errno));
}

const char* strprintf(const char* fmt, ...)
{
	va_list args1;
	va_start(args1, fmt);
	va_list args2;
	va_copy(args2, args1);

	int len = vsnprintf(NULL, 0, fmt, args1);
	if (len < 0) {
		fprintf(stderr, "%s: vsnprintf '%s' failed\n", argv0, fmt);
		exit(EXIT_EXECERROR);
	}

	char* buf = malloc(len + 1);
	if (!buf) {
		fprintf(stderr, "%s: malloc %d\n", argv0, len + 1);
		exit(EXIT_EXECERROR);
	}

	va_end(args1);

	if (vsnprintf(buf, len + 1, fmt, args2) != len) {
		fprintf(stderr, "%s: vsnprintf '%s' returned unexpected length\n", argv0, fmt);
		exit(EXIT_EXECERROR);
	}

	va_end(args2);

	return buf;
}

static void* xrealloc(void* ptr, size_t size)
{
	ptr = realloc(ptr, size);
	if (!ptr) {
		fprintf(stderr, "%s: realloc %zu\n", argv0, size);
		exit(EXIT_EXECERROR);
	}
	return ptr;
}

static int write_to(const char* path, const char* fmt, ...)
{
	int fd = open(path, O_WRONLY);
	if (fd >= 0) {
		va_list args;
		va_start(args, fmt);
		if (vdprintf(fd, fmt, args) < 0) {
			va_end(args);
			close(fd);
			return 1;
		}
		va_end(args);
		close(fd);
		return 0;
	}
	return 1;
}

// read an entire file into a NUL-terminated buffer
static char* read_file(const char* path, size_t* size_out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}

	size_t size = 0, cap = 0;
	char* buf = NULL;
	for (;;) {
		if (cap - size < 4096) {
			cap = cap ? cap * 2 : 65536;
			buf = xrealloc(buf, cap + 1);
		}
		ssize_t n = read(fd, buf + size, cap - size);
		if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0) {
			free(buf);
			close(fd);
			return NULL;
		} else if (n == 0) {
			break;
		}
		size += n;
	}
	close(fd);

	buf[size] = 0;
	if (size_out) {
		*size_out = size;
	}
	return buf;
}

// readlink, but returns a malloc'd string (or NULL on failure)
static char* read_link(const char* path)
{
	char buf[PATH_MAX + 1];
	ssize_t len = readlink(path, buf, PATH_MAX);
	if (len < 0) {
		return NULL;
	}
	buf[len] = 0;
	return strdup(buf);
}

// Make the entries of the directory from_dir (e.g. /) also visible under
// to_dir (e.g. <mountroot>), except for the ones listed in skip.
static void bind_entries(const char* from_dir, const char* to_dir, const char* const* skip)
{
	DIR* dir = opendir(from_dir);
	if (!dir) {
		if (errno != ENOENT) {
			warn("opendir %s", from_dir);
		}
		return;
	}

	struct dirent* entry;
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		bool skipped = false;
		for (const char* const* s = skip; s && *s; ++s) {
			skipped = skipped || strcmp(entry->d_name, *s) == 0;
		}
		if (skipped) {
			continue;
		}

		const char* from = strprintf("%s/%s", strcmp(from_dir, "/") == 0 ? "" : from_dir, entry->d_name);
		const char* to = strprintf("%s/%s", to_dir, entry->d_name);

		// we don't treat failure of the below bind as an actual failure, since
		// our logic not robust enough to handle weird filesystem scenarios

		struct stat statbuf;
		if (lstat(from, &statbuf) == 0 && S_ISLNK(statbuf.st_mode)) {
			// imitate symlinks as symlinks, so they resolve the same way
			// inside the new root
			char* target = read_link(from);
			if (!target || symlink(target, to) < 0) {
				warn("symlink %s -> %s", to, target ? target : from);
			}
			free(target);
		} else if (stat(from, &statbuf) < 0) {
			warn("stat %s", from);
		} else if (S_ISDIR(statbuf.st_mode)) {
			die_if(mkdir(to, statbuf.st_mode & ~S_IFMT) < 0, "mkdir %s", to);
			if (mount(from, to, "none", MS_BIND | MS_REC, 0) < 0) {
				warn("mount %s -> %s", from, to);
			}
		} else {
			// effectively touch
			int fd = creat(to, statbuf.st_mode & ~S_IFMT);
			if (fd == -1) {
				warn("creat %s", to);
			} else {
				close(fd);
				if (mount(from, to, "none", MS_BIND | MS_REC, 0) < 0) {
					warn("mount %s -> %s", from, to);
				}
			}
		}

		free((void*) from);
		free((void*) to);
	}

	closedir(dir);
}

// Graphics drivers ----------------------------------------------------------
//
// Nix-built programs look for GPU drivers in /run/opengl-driver, which is where
// NixOS puts them. Other distros don't have that, so if the AppImage was built
// with graphics support (see mkAppImage.nix), we create it ourselves in our
// mount namespace:
//
// - Bundled drivers (usually Mesa, which supports Intel, AMD, Nouveau, virtual
//   GPUs and software rendering) are symlinked into /run/opengl-driver.
//
// - NVIDIA's proprietary driver can't be bundled, since its userspace has to
//   exactly match the host's kernel module. Instead, we find the host's NVIDIA
//   libraries through the host's ld.so cache, and make them available to nix
//   programs through the ld.so cache of the bundled glibc(s). Unlike
//   LD_LIBRARY_PATH, this doesn't affect host programs (they use the host's
//   glibc, which reads /etc/ld.so.cache instead), and is only consulted after
//   a library's RUNPATH (so nix programs keep using their own libraries).
//
//   The host's libraries don't have a RUNPATH, so their dependencies (other
//   than glibc and each other) are added to the cache too. These come from
//   the bundled host-driver-deps.
//
// We do this even if the host has its own /run/opengl-driver (e.g. it's NixOS),
// since that points into the host's /nix/store, which we hide.

#define DRIVER_LINK "/run/opengl-driver"

struct graphics_config {
	char* drivers; // store path to bundled drivers
	char* host_deps; // store path to libraries needed by host drivers
	char* cache_targets; // newline-separated ld.so.cache paths of nix glibcs
};

// Read <appdir>/graphics, which is present when the AppImage bundles graphics
// drivers. Returns false if the AppImage doesn't bundle graphics drivers.
static bool read_graphics_config(struct graphics_config* config)
{
	const char* dir = strprintf("%s/graphics", appdir);
	const char* drivers = strprintf("%s/drivers", dir);
	const char* host_deps = strprintf("%s/host-driver-deps", dir);
	const char* cache_targets = strprintf("%s/ld.so.cache-targets", dir);

	// these are symlinks to /nix/store paths, which we can only resolve after
	// we've changed root
	config->drivers = read_link(drivers);
	config->host_deps = read_link(host_deps);
	config->cache_targets = read_file(cache_targets, NULL);

	free((void*) dir);
	free((void*) drivers);
	free((void*) host_deps);
	free((void*) cache_targets);

	return config->drivers != NULL;
}

// Fill to with a tree of directories mirroring from, with symlinks to the files
// within it
static void mirror_tree(const char* from, const char* to)
{
	DIR* dir = opendir(from);
	if (!dir) {
		warn("opendir %s", from);
		return;
	}

	struct dirent* entry;
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}

		const char* from_entry = strprintf("%s/%s", from, entry->d_name);
		const char* to_entry = strprintf("%s/%s", to, entry->d_name);

		struct stat statbuf;
		if (stat(from_entry, &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
			if (mkdir(to_entry, 0755) < 0 && errno != EEXIST) {
				warn("mkdir %s", to_entry);
			} else {
				mirror_tree(from_entry, to_entry);
			}
		} else if (symlink(from_entry, to_entry) < 0) {
			warn("symlink %s -> %s", to_entry, from_entry);
		}

		free((void*) from_entry);
		free((void*) to_entry);
	}

	closedir(dir);
}

// ld.so cache ---------------------------------------------------------------
//
// See glibc's sysdeps/generic/dl-cache.h for the format. In short, the file
// (optionally preceded by an old-format cache, which we ignore) has a header,
// an array of entries, then a string table. Entries map library names to
// paths, and are sorted in descending order (according to libcmp below).

#if defined(__x86_64__) && defined(__LP64__)
#define CACHE_FLAGS 0x0303 // FLAG_ELF_LIBC6 | FLAG_X8664_LIB64
#elif defined(__aarch64__) && defined(__LP64__)
#define CACHE_FLAGS 0x0a03 // FLAG_ELF_LIBC6 | FLAG_AARCH64_LIB64
#elif defined(__i386__)
#define CACHE_FLAGS 0x0003 // FLAG_ELF_LIBC6
#endif

#ifdef CACHE_FLAGS

#define CACHE_MAGIC_OLD "ld.so-1.7.0"
#define CACHE_MAGIC_NEW "glibc-ld.so.cache1.1"

struct cache_entry_old {
	int32_t flags;
	uint32_t key, value;
};

struct cache_header_old {
	char magic[sizeof CACHE_MAGIC_OLD - 1];
	uint32_t nlibs;
	struct cache_entry_old libs[];
};

struct cache_entry_new {
	int32_t flags;
	uint32_t key, value; // offsets from start of cache_header_new
	uint32_t osversion_unused;
	uint64_t hwcap;
};

struct cache_header_new {
	char magic[sizeof CACHE_MAGIC_NEW - 1];
	uint32_t nlibs;
	uint32_t len_strings;
	uint8_t flags;
	uint8_t padding_unused[3];
	uint32_t extension_offset;
	uint32_t unused[3];
	struct cache_entry_new libs[];
};

#define CACHE_ENDIAN_MASK 3
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CACHE_ENDIAN_CURRENT 2
#else
#define CACHE_ENDIAN_CURRENT 3
#endif

struct lib {
	const char* name;
	const char* path;
};

struct libs {
	struct lib* items;
	size_t len, cap;
};

static void libs_push(struct libs* libs, const char* name, const char* path)
{
	if (libs->len == libs->cap) {
		libs->cap = libs->cap ? libs->cap * 2 : 64;
		libs->items = xrealloc(libs->items, libs->cap * sizeof(struct lib));
	}
	libs->items[libs->len++] = (struct lib) { name, path };
}

static const char* libs_find(const struct libs* libs, const char* name)
{
	for (size_t i = 0; i < libs->len; ++i) {
		if (strcmp(libs->items[i].name, name) == 0) {
			return libs->items[i].path;
		}
	}
	return NULL;
}

// Compare library names the same way glibc does (_dl_cache_libcmp), which
// compares runs of digits numerically
static int libcmp(const char* p1, const char* p2)
{
	while (*p1 != '\0') {
		if (*p1 >= '0' && *p1 <= '9') {
			if (*p2 >= '0' && *p2 <= '9') {
				int val1 = *p1++ - '0';
				int val2 = *p2++ - '0';
				while (*p1 >= '0' && *p1 <= '9') {
					val1 = val1 * 10 + *p1++ - '0';
				}
				while (*p2 >= '0' && *p2 <= '9') {
					val2 = val2 * 10 + *p2++ - '0';
				}
				if (val1 != val2) {
					return val1 - val2;
				}
			} else {
				return 1;
			}
		} else if (*p2 >= '0' && *p2 <= '9') {
			return -1;
		} else if (*p1 != *p2) {
			return *p1 - *p2;
		} else {
			++p1;
			++p2;
		}
	}
	return *p1 - *p2;
}

static int lib_compare_desc(const void* a, const void* b)
{
	return libcmp(((const struct lib*) b)->name, ((const struct lib*) a)->name);
}

// Find the new-format header within an ld.so cache, or NULL if there isn't a
// usable one
static const struct cache_header_new* cache_find_header(const char* data, size_t size)
{
	const struct cache_header_new* header = NULL;
	if (size >= sizeof(struct cache_header_new) && memcmp(data, CACHE_MAGIC_NEW, sizeof CACHE_MAGIC_NEW - 1) == 0) {
		header = (const struct cache_header_new*) data;
	} else if (size >= sizeof(struct cache_header_old) && memcmp(data, CACHE_MAGIC_OLD, sizeof CACHE_MAGIC_OLD - 1) == 0) {
		// old format, which may be followed by the new format
		const struct cache_header_old* old = (const struct cache_header_old*) data;
		if (old->nlibs > (size - sizeof *old) / sizeof(struct cache_entry_old)) {
			return NULL;
		}

		size_t align = _Alignof(struct cache_header_new);
		size_t offset = (sizeof *old + old->nlibs * sizeof(struct cache_entry_old) + align - 1) & ~(align - 1);
		if (offset + sizeof(struct cache_header_new) <= size && memcmp(data + offset, CACHE_MAGIC_NEW, sizeof CACHE_MAGIC_NEW - 1) == 0) {
			header = (const struct cache_header_new*) (data + offset);
		}
	}

	if (!header) {
		return NULL;
	}

	size_t avail = size - ((const char*) header - data);
	if (header->nlibs > (avail - sizeof *header) / sizeof(struct cache_entry_new)) {
		return NULL;
	}
	if (header->flags != 0 && (header->flags & CACHE_ENDIAN_MASK) != CACHE_ENDIAN_CURRENT) {
		return NULL;
	}

	return header;
}

// Add libraries from the ld.so cache at path whose names start with one of
// prefixes. The data is kept alive for the lifetime of the program.
static void cache_read(const char* path, const char* const* prefixes, struct libs* out)
{
	size_t size;
	char* data = read_file(path, &size);
	if (!data) {
		return;
	}

	const struct cache_header_new* header = cache_find_header(data, size);
	if (!header) {
		free(data);
		return;
	}

	const char* strings = (const char*) header;
	size_t strings_size = size - (strings - data);
	for (uint32_t i = 0; i < header->nlibs; ++i) {
		const struct cache_entry_new* entry = &header->libs[i];
		if (entry->flags != CACHE_FLAGS || entry->hwcap != 0) {
			// different architecture, or for glibc-hwcaps subdirectories
			continue;
		}
		if (entry->key >= strings_size || entry->value >= strings_size) {
			continue;
		}

		// read_file NUL-terminates data, so these are terminated too
		const char* name = strings + entry->key;
		const char* lib_path = strings + entry->value;

		for (const char* const* prefix = prefixes; *prefix; ++prefix) {
			if (strncmp(name, *prefix, strlen(*prefix)) == 0) {
				if (!libs_find(out, name) && access(lib_path, F_OK) == 0) {
					libs_push(out, name, lib_path);
				}
				break;
			}
		}
	}
}

// Write an ld.so cache containing libs. This sorts libs.
static bool cache_write(const char* path, struct libs* libs)
{
	qsort(libs->items, libs->len, sizeof(struct lib), lib_compare_desc);

	// glibc does a binary search, so names need to be unique
	size_t n = 0;
	for (size_t i = 0; i < libs->len; ++i) {
		if (n == 0 || libcmp(libs->items[n - 1].name, libs->items[i].name) != 0) {
			libs->items[n++] = libs->items[i];
		}
	}
	libs->len = n;

	size_t strings_offset = sizeof(struct cache_header_new) + n * sizeof(struct cache_entry_new);
	size_t strings_size = 0;
	for (size_t i = 0; i < n; ++i) {
		strings_size += strlen(libs->items[i].name) + 1 + strlen(libs->items[i].path) + 1;
	}

	size_t size = strings_offset + strings_size;
	char* data = calloc(1, size);
	if (!data) {
		return false;
	}

	struct cache_header_new* header = (struct cache_header_new*) data;
	memcpy(header->magic, CACHE_MAGIC_NEW, sizeof CACHE_MAGIC_NEW - 1);
	header->nlibs = n;
	header->len_strings = strings_size;
	header->flags = CACHE_ENDIAN_CURRENT;

	size_t offset = strings_offset;
	for (size_t i = 0; i < n; ++i) {
		header->libs[i].flags = CACHE_FLAGS;

		header->libs[i].key = offset;
		strcpy(data + offset, libs->items[i].name);
		offset += strlen(libs->items[i].name) + 1;

		header->libs[i].value = offset;
		strcpy(data + offset, libs->items[i].path);
		offset += strlen(libs->items[i].path) + 1;
	}

	bool ok = false;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	if (fd >= 0) {
		ok = write(fd, data, size) == (ssize_t) size;
		ok = close(fd) == 0 && ok;
	}

	free(data);
	return ok;
}

// symlink to -> from, creating the parent directory of to if needed
static void link_into(const char* from, const char* to)
{
	char* parent = strdup(to);
	if (mkdir(dirname(parent), 0755) < 0 && errno != EEXIST) {
		warn("mkdir %s", parent);
	}
	free(parent);

	if (symlink(from, to) < 0 && errno != EEXIST) {
		warn("symlink %s -> %s", to, from);
	}
}

// The host's NVIDIA libraries. See e.g. the nvidia-x11 package in nixpkgs.
static const char* const nvidia_prefixes[] = {
	"libGLX_nvidia.so.",
	"libEGL_nvidia.so.",
	"libGLESv1_CM_nvidia.so.",
	"libGLESv2_nvidia.so.",
	"libnvidia-",
	"libcuda.so",
	"libcudadebugger.so.",
	"libnvcuvid.so.",
	"libnvoptix.so.",
	"libvdpau_nvidia.so.",
	NULL,
};

static void setup_nvidia(const struct graphics_config* config)
{
	struct libs libs = { 0 };

	cache_read("/etc/ld.so.cache", nvidia_prefixes, &libs);
	if (libs.len == 0) {
		return;
	}

	// symlink everything into /run/opengl-driver/lib, like on NixOS. Most
	// things will find the libraries through the ld.so cache, but some things
	// look there directly (e.g. libGLX finds libGLX_nvidia through its RUNPATH)
	for (size_t i = 0; i < libs.len; ++i) {
		const char* to = strprintf(DRIVER_LINK "/lib/%s", libs.items[i].name);
		link_into(libs.items[i].path, to);
		free((void*) to);
	}

	// GBM backend, which is libnvidia-allocator under a different name
	const char* allocator = libs_find(&libs, "libnvidia-allocator.so.1");
	if (allocator) {
		link_into(allocator, DRIVER_LINK "/lib/gbm/nvidia-drm_gbm.so");
	}

	// VDPAU driver, which is usually in a subdirectory that isn't in the ld.so
	// cache
	const char* vdpau = libs_find(&libs, "libvdpau_nvidia.so.1");
	const char* glx = libs_find(&libs, "libGLX_nvidia.so.0");
	if (!vdpau && glx) {
		char* dir = strdup(glx);
		vdpau = strprintf("%s/vdpau/libvdpau_nvidia.so.1", dirname(dir));
		free(dir);
	}
	if (vdpau && access(vdpau, F_OK) == 0) {
		link_into(vdpau, DRIVER_LINK "/lib/vdpau/libvdpau_nvidia.so.1");
	}

	// the libraries from nixpkgs that host drivers need
	if (config->host_deps) {
		const char* deps_dir = strprintf("%s/lib", config->host_deps);
		DIR* dir = opendir(deps_dir);
		if (!dir) {
			warn("opendir %s", deps_dir);
		} else {
			struct dirent* entry;
			while ((entry = readdir(dir))) {
				if (entry->d_name[0] != '.') {
					libs_push(&libs, strdup(entry->d_name), strprintf("%s/%s", deps_dir, entry->d_name));
				}
			}
			closedir(dir);
		}
	}

	const char* cache = DRIVER_LINK "/ld.so.cache";
	if (!cache_write(cache, &libs)) {
		warn("cannot write %s", cache);
		return;
	}

	// replace the (empty) ld.so caches of the bundled glibcs
	if (!config->cache_targets) {
		return;
	}
	for (char* target = strtok(config->cache_targets, "\n"); target; target = strtok(NULL, "\n")) {
		if (mount(cache, target, "none", MS_BIND, 0) < 0) {
			warn("mount %s -> %s", cache, target);
		}
	}
}

#else

static void setup_nvidia(const struct graphics_config* config)
{
	(void) config;
}

#endif

// Called after changing root, with /run being our tmpfs
static void setup_graphics(const struct graphics_config* config)
{
	if (mkdir(DRIVER_LINK, 0755) < 0) {
		warn("mkdir " DRIVER_LINK);
		return;
	}

	mirror_tree(config->drivers, DRIVER_LINK);
	setup_nvidia(config);
}

void child_main(char** argv)
{
	// get uid, gid before going to new namespace
	uid_t uid = getuid();
	gid_t gid = getgid();

	struct graphics_config graphics = { 0 };
	bool provide_graphics = read_graphics_config(&graphics);

	// Create new mount namespace, and a user namespace unless we're root (so that
	// we can mount() in userland)
	bool userns = uid != 0;
	if (!userns && unshare(CLONE_NEWNS) < 0) {
		// we're root without CAP_SYS_ADMIN (e.g. in a container), so do the same
		// as for other users
		die_if(errno != EPERM, "cannot unshare");
		userns = true;
	}

	if (userns) {
		die_if(unshare(CLONE_NEWNS | CLONE_NEWUSER) < 0, "cannot unshare (are unprivileged user namespaces disabled?)");

		// UID/GID Mapping -----------------------------------------------------------

		// see user_namespaces(7)
		// > The data written to uid_map (gid_map) must consist of a single line that
		// > maps the writing process's effective user ID (group ID) in the parent
		// > user namespace to a user ID (group ID) in the user namespace.
		die_if(write_to("/proc/self/uid_map", "%u %u 1\n", uid, uid), "cannot write uid_map");

		// see user_namespaces(7):
		// > In the case of gid_map, use of the setgroups(2) system call must first
		// > be denied by writing "deny" to the /proc/[pid]/setgroups file (see
		// > below) before writing to gid_map.
		die_if(write_to("/proc/self/setgroups", "deny"), "cannot write setgroups");
		die_if(write_to("/proc/self/gid_map", "%u %u 1\n", gid, gid), "cannot write gid_map");
	}

	// Mountpoint ----------------------------------------------------------------

	// Stop our mounts from propagating back to the host's mount namespace, while
	// still seeing the host's new mounts (e.g. a USB drive being plugged in).
	// This already happens if we created a user namespace, but not as root.
	//
	// EINVAL means / isn't a mount point (e.g. we're in a chroot). All our mounts
	// are under appdir, so it's enough to do this for that instead, though it's
	// only a mount point if the runtime mounted the AppImage rather than
	// extracting it.
	if (mount("none", "/", 0, MS_REC | MS_SLAVE, 0) < 0) {
		if (errno != EINVAL) {
			warn("cannot make mounts slaves");
		} else if (mount("none", appdir, 0, MS_REC | MS_SLAVE, 0) < 0 && errno != EINVAL) {
			warn("cannot make %s a slave mount", appdir);
		}
	}

	// tmpfs so we don't need to cleanup
	if (mount("tmpfs", mountroot, "tmpfs", 0, "mode=755") < 0) {
		// this is the first thing that needs the user namespace's capabilities,
		// which some systems deny, e.g. Ubuntu since 23.10 through AppArmor
		die_if(userns && errno == EPERM, "mount tmpfs -> %s (is what unprivileged user namespaces can do restricted, e.g. by kernel.apparmor_restrict_unprivileged_userns?)", mountroot);
		die_if(true, "mount tmpfs -> %s", mountroot);
	}
	// make unbindable to both prevent event propagation as well as mount explosion
	die_if(mount(mountroot, mountroot, "none", MS_UNBINDABLE, 0) < 0, "mount tmpfs bind -> %s", mountroot);

	// copy over root directories, apart from /nix (which we replace) and /run
	// (which we need to add /run/opengl-driver to)
	const char* const root_skip[] = { "nix", provide_graphics ? "run" : NULL, NULL };
	bind_entries("/", mountroot, root_skip);

	if (provide_graphics) {
		// a tmpfs rather than a directory in mountroot, since things in an
		// unbindable mount can't be bind-mounted elsewhere
		const char* run_to = strprintf("%s/run", mountroot);
		die_if(mkdir(run_to, 0755) < 0, "mkdir %s", run_to);
		die_if(mount("tmpfs", run_to, "tmpfs", 0, "mode=755") < 0, "mount tmpfs -> %s", run_to);
		const char* const run_skip[] = { "opengl-driver", NULL };
		bind_entries("/run", run_to, run_skip);
		free((void*) run_to);
	}

	// mount in /nix
	const char* nix_from = strprintf("%s/nix", appdir);
	const char* nix_to = strprintf("%s/nix", mountroot);

	die_if(mkdir(nix_to, 0777) < 0, "mkdir %s", nix_to);
	die_if(mount(nix_from, nix_to, "none", MS_BIND | MS_REC, 0) < 0, "mount %s -> %s", nix_from, nix_to);

	free((void*) nix_from);
	free((void*) nix_to);

	// Writes to the root would otherwise silently go to our tmpfs. This doesn't
	// affect the mounts within it.
	die_if(mount(mountroot, mountroot, "none", MS_REMOUNT | MS_BIND | MS_RDONLY, 0) < 0, "cannot make %s read-only", mountroot);

	// Change root ---------------------------------------------------------------

	// save where we were so we can cd into it
	char cwd_buf[PATH_MAX];
	const char* cwd = getcwd(cwd_buf, PATH_MAX);
	if (!cwd) {
		warn("cannot getcwd");
	}

	// Use pivot_root rather than chroot, since the kernel doesn't let chrooted
	// processes create user namespaces, which e.g. bwrap and Chromium's sandbox
	// need. Passing "." for both arguments puts the old root on top of the new
	// one, which we then detach (see pivot_root(2)).
	//
	// pivot_root doesn't work in some situations (e.g. if the current root is
	// the initramfs), so fall back to chroot.
	die_if(chdir(mountroot) < 0, "cannot chdir %s", mountroot);
	if (syscall(SYS_pivot_root, ".", ".") == 0) {
		die_if(umount2(".", MNT_DETACH) < 0, "cannot detach old root");
	} else {
		die_if(chroot(mountroot) < 0, "cannot chroot %s", mountroot);
	}

	// cd back again. This can fail even though we were there, e.g. if it's been
	// deleted, or we can't access it (e.g. `sudo -u user` from a directory only
	// root can access), in which case fall back to $HOME or / like bwrap does.
	if (!cwd || chdir(cwd) < 0) {
		if (cwd) {
			warn("cannot chdir %s", cwd);
		}
		const char* home = getenv("HOME");
		if (!home || chdir(home) < 0) {
			die_if(chdir("/") < 0, "cannot chdir /");
		}
	}

	// Graphics ------------------------------------------------------------------

	if (provide_graphics) {
		setup_graphics(&graphics);

		// like for the root, writes directly to /run would otherwise silently go
		// to our tmpfs. This doesn't affect the mounts within it.
		if (mount("/run", "/run", "none", MS_REMOUNT | MS_BIND | MS_RDONLY, 0) < 0) {
			warn("cannot make /run read-only");
		}
	}

	// Exec ----------------------------------------------------------------------

	// For better error messages, we wanna get what entrypoint points to
	const char* entrypoint = strprintf("%s/entrypoint", appdir);
	char exe[PATH_MAX + 1];
	ssize_t exe_size = readlink(entrypoint, exe, PATH_MAX);
	die_if(exe_size < 0, "cannot read link %s", entrypoint);
	exe[exe_size] = 0;

	// argv[0] is the AppImage (or, with --appimage-extract-and-run, this
	// AppRun), but programs expect it to be themselves, e.g. multi-call
	// binaries like coreutils use it to decide what to do. The runtime puts the
	// AppImage's path in $APPIMAGE and its argv[0] in $ARGV0 instead.
	argv[0] = exe;
	execv(exe, argv);
	die_if(true, "cannot exec %s", exe);
}

int main(int argc, char** argv)
{
	argv0 = argv[0];

	// get location of exe
	char appdir_buf[PATH_MAX];
	die_if(!realpath("/proc/self/exe", appdir_buf), "cannot access /proc/self/exe");
	appdir = dirname(appdir_buf);

	// use <appdir>/mountpoint as alternate root. Since this already exists
	// inside the squashfs, we don't need to remove this dir later (which we
	// would have had to do if using mktemp)!
	mountroot = strprintf("%s/mountroot", appdir);

	child_main(argv);
}
