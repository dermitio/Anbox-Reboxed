#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../ashmem/uapi/ashmem.h"

static void fail(const char *operation)
{
	fprintf(stderr, "%s: %s\n", operation, strerror(errno));
	exit(EXIT_FAILURE);
}

static int open_area(const char *name, size_t size)
{
	int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		fail("open /dev/ashmem");
	if (ioctl(fd, ASHMEM_SET_NAME, name) < 0)
		fail("ASHMEM_SET_NAME");
	if (ioctl(fd, ASHMEM_SET_SIZE, size) < 0)
		fail("ASHMEM_SET_SIZE");
	return fd;
}

int main(void)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	const size_t size = (size_t)page_size * 8;
	struct ashmem_pin pin = {
		.offset = 0,
		.len = (uint32_t)size,
	};
	unsigned char expected[32];
	unsigned char actual[sizeof(expected)];
	unsigned char *mapping;
	void *invalid;
	pid_t child;
	int fd;
	int prot_fd;
	int status;
	int pin_status;

	if (page_size <= 0 || size > UINT32_MAX) {
		errno = EOVERFLOW;
		fail("page size");
	}

	fd = open_area("ashmem-smoke", size);

	errno = 0;
	invalid = mmap(NULL, size + (size_t)page_size,
		       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (invalid != MAP_FAILED || errno != EINVAL) {
		fprintf(stderr, "oversized mmap was not rejected with EINVAL\n");
		return EXIT_FAILURE;
	}

	errno = 0;
	invalid = mmap(NULL, (size_t)page_size, PROT_READ,
		       MAP_SHARED, fd, (off_t)size);
	if (invalid != MAP_FAILED || errno != EINVAL) {
		fprintf(stderr, "out-of-range mmap offset was not rejected\n");
		return EXIT_FAILURE;
	}

	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		fail("shared mmap");

	for (size_t i = 0; i < sizeof(expected); ++i) {
		expected[i] = (unsigned char)(0x40U + i);
		mapping[i] = expected[i];
	}

	memset(actual, 0, sizeof(actual));
	if (pread(fd, actual, sizeof(actual), 0) != (ssize_t)sizeof(actual))
		fail("pread");
	if (memcmp(expected, actual, sizeof(expected)) != 0) {
		fprintf(stderr, "pread did not observe mapped data\n");
		return EXIT_FAILURE;
	}

	child = fork();
	if (child < 0)
		fail("fork");
	if (child == 0) {
		if (memcmp(expected, mapping, sizeof(expected)) != 0)
			_exit(2);
		mapping[page_size] = 0xa5;
		_exit(0);
	}
	if (waitpid(child, &status, 0) < 0)
		fail("waitpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
	    mapping[page_size] != 0xa5) {
		fprintf(stderr, "forked shared mapping check failed\n");
		return EXIT_FAILURE;
	}

	if (ioctl(fd, ASHMEM_UNPIN, &pin) < 0)
		fail("ASHMEM_UNPIN");
	if (ioctl(fd, ASHMEM_PURGE_ALL_CACHES, 0) < 0 && errno != EPERM)
		fail("ASHMEM_PURGE_ALL_CACHES");
	pin_status = ioctl(fd, ASHMEM_PIN, &pin);
	if (pin_status < 0)
		fail("ASHMEM_PIN");

	close(fd);
	mapping[page_size * 2] = 0x5a;
	if (mapping[page_size * 2] != 0x5a) {
		fprintf(stderr, "mapping did not survive parent fd close\n");
		return EXIT_FAILURE;
	}
	if (munmap(mapping, size) < 0)
		fail("munmap");

	prot_fd = open_area("ashmem-protection", (size_t)page_size);
	if (ioctl(prot_fd, ASHMEM_SET_PROT_MASK, PROT_READ) < 0)
		fail("ASHMEM_SET_PROT_MASK");
	errno = 0;
	invalid = mmap(NULL, (size_t)page_size,
		       PROT_READ | PROT_WRITE, MAP_SHARED, prot_fd, 0);
	if (invalid != MAP_FAILED || errno != EPERM) {
		fprintf(stderr, "write mapping bypassed ashmem protection mask\n");
		return EXIT_FAILURE;
	}
	close(prot_fd);

	printf("ashmem smoke test passed (pin status %d)\n", pin_status);
	return EXIT_SUCCESS;
}
