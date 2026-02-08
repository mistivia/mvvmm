#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <termios.h>

#include "config.h"
#include "mvvm.h"
#include "serial.h"

struct termios orig_termios;
int g_term_changed = 0;

void reset_terminal_mode() {
    tcsetattr(0, TCSAFLUSH, &orig_termios);
}

void set_terminal_raw_mode() {
    struct termios raw = {0};
    if (!isatty(0)) {
        fprintf(stderr, "Not a terminal.\n");
        return;
    }
    tcgetattr(0, &orig_termios);
    atexit(reset_terminal_mode);
    
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1; // return at 1 char
    raw.c_cc[VTIME] = 0; // no timeout
    tcsetattr(0, TCSAFLUSH, &raw);
    g_term_changed = 1;
}

void* keyboard_thread_func(void* arg) {
    fprintf(stderr, "Press Ctrl+A & Ctrl+C to exit...\n");
    struct mvvm *vm = arg;
    set_terminal_raw_mode();
    int ch = 0;
    int escaped = 0;
    while ((ch = getchar()) != EOF) {
        if (escaped) {
            escaped = 0;
            if (ch == 0x03) {
                kill(getpid(), SIGINT);
                // vm->quit = true;
                // break;
            } else if (ch == 0x01) {
                write_to_serial(vm, (char)0x01);
            } else {
                write_to_serial(vm, (char)0x01);
                write_to_serial(vm, (char)ch);
            }
        } else {
            if (ch == 0x01) {
                escaped = 1;
            } else {
                write_to_serial(vm, (char)ch);
            }
        }
    }
    return NULL;
}

void sigint_handler(int sig) {
    (void)sig;
    if (g_term_changed) {
        reset_terminal_mode();
    }
    _exit(128 + SIGINT);
}

struct cmd_opts {
    const char *kernel_path;
    const char *initrd_path; // can be null
    const char *disk_path; // can be null
    uint64_t memory_size; // default 1GB
    const char *kernel_cmdline; // default "console=ttyS0 debug"
    const char *tap_ifname;
};

static void print_usage(FILE *stream, const char *program_name);

// Parse memory size string supporting optional K/M/G suffix
// Returns 0 on success, -1 on error
static int
parse_memory_size(const char *str, uint64_t *result)
{
    char *endptr = NULL;
    unsigned long long val = -1;
    uint64_t multiplier = 1;

    errno = 0;
    val = strtoull(str, &endptr, 0);

    if (errno == ERANGE || errno == EINVAL) {
        return -1;
    }

    if (endptr == str) {
        // No digits found
        return -1;
    }

    if (*endptr != '\0') {
        // Process size suffix
        switch (*endptr) {
        case 'k':
        case 'K':
            multiplier = 1024;
            break;
        case 'm':
        case 'M':
            multiplier = 1024 * 1024;
            break;
        case 'g':
        case 'G':
            multiplier = 1024ULL * 1024 * 1024;
            break;
        default:
            return -1;
        }

        if (*(endptr + 1) != '\0') {
            // Extra characters after suffix
            return -1;
        }

        // Check for overflow before multiplication
        if (val > UINT64_MAX / multiplier) {
            return -1;
        }
        val *= multiplier;
    }

    *result = (uint64_t)val;
    return 0;
}

struct cmd_opts
parse_opts(int argc, char **argv)
{
    struct cmd_opts opts = {
        .kernel_path = NULL,
        .initrd_path = NULL,
        .disk_path = NULL,
        .tap_ifname = NULL,
        .memory_size = 1024LL * 1024 * 1024,
        .kernel_cmdline = DEFAULT_KERNEL_CMDLINE
    };

    int opt = 0;
    const char *program_name = (argc > 0) ? argv[0] : "mvvmm";

    // Suppress getopt's default error messages for manual handling
    opterr = 0;

    while ((opt = getopt(argc, argv, "k:i:m:a:h:d:t:")) != -1) {
        switch (opt) {
        case 'k':
            opts.kernel_path = optarg;
            break;
        case 'i':
            opts.initrd_path = optarg;
            break;
        case 'd':
            opts.disk_path = optarg;
            break;
        case 't':
            opts.tap_ifname = optarg;
            break;
        case 'm': {
            uint64_t mem_size;
            if (parse_memory_size(optarg, &mem_size) != 0) {
                fprintf(stderr, "Error: Invalid memory size '%s'\n",
                        optarg);
                print_usage(stderr, program_name);
                exit(EXIT_FAILURE);
            }
            opts.memory_size = mem_size;
            break;
        }
        case 'a':
            opts.kernel_cmdline = optarg;
            break;
        case 'h':
            print_usage(stdout, program_name);
            exit(EXIT_SUCCESS);
        case '?':
            if (optopt == 'k' || optopt == 'i'
                    || optopt == 'm' || optopt == 'a'
                    || optopt == 'd' || optopt == 't') {
                fprintf(stderr,
                        "Error: Option -%c requires an argument.\n",
                        optopt);
            } else {
                fprintf(stderr,
                        "Error: Unknown option '-%c'.\n",
                        optopt);
            }
            print_usage(stderr, program_name);
            exit(EXIT_FAILURE);
        default:
            print_usage(stderr, program_name);
            exit(EXIT_FAILURE);
        }
    }

    // Reject extra non-option arguments
    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected argument '%s'\n",
                argv[optind]);
        print_usage(stderr, program_name);
        exit(EXIT_FAILURE);
    }

    // Validate required arguments
    if (opts.kernel_path == NULL) {
        fprintf(stderr, "Error: Kernel path (-k) is required.\n");
        print_usage(stderr, program_name);
        exit(EXIT_FAILURE);
    }

    return opts;
}

static void
print_usage(FILE *stream, const char *program_name)
{
    fprintf(stream,
            "Usage: %s -k VMLINUZ [-i INITRD] [-m MEMORY_SIZE] "
            "[-a KERNEL_CMDLINE] [-d DISK_IMAGE]\n",
            program_name);
    fprintf(stream, "\n");
    fprintf(stream, "Options:\n");
    fprintf(stream,
            "  -k VMLINUZ        Path to kernel image (required)\n");
    fprintf(stream,
            "  -i INITRD         Path to initrd image (optional)\n");
    fprintf(stream,
            "  -m MEMORY_SIZE    Memory size with optional K/M/G suffix "
            "(default: 1G)\n");
    fprintf(stream,
            "  -d DISK_IMG       Path to disk image (optional)\n");
    fprintf(stream,
            "  -t TAP_IFNAME     Tap interface name (optional)\n");
    fprintf(stream,
            "  -a KERNEL_CMDLINE Kernel command line "
            "(default: \"console=ttyS0 debug\")\n");
    fprintf(stream,
            "  -h                Show this help message\n");
}

int main(int argc, char *argv[]) {
    struct mvvm vm = {0};
    struct cmd_opts opts = parse_opts(argc, argv);
    signal(SIGINT, sigint_handler);
    if (mvvm_init(&vm, opts.memory_size, opts.disk_path, opts.tap_ifname) < 0) {
        return -1;
    }
    if (mvvm_load_kernel(&vm, opts.kernel_path, opts.initrd_path, opts.kernel_cmdline) < 0) {
        return -1;
    }
    pthread_t keyboard_thread = {0};
    if (pthread_create(&keyboard_thread, NULL, keyboard_thread_func, &vm) != 0) {
        perror("Failed to create thread");
        return 1;
    }
    int ret = -1;
    if ((ret = mvvm_run(&vm)) != 0) {
        fprintf(stderr, "mvvm exit code: %d\n", ret);
        return ret;
    }
    pthread_join(keyboard_thread, NULL);
    mvvm_destroy(&vm);
    return 0;
}