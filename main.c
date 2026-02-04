#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <termios.h>

#include "mvvm.h"
#include "serial.h"

struct termios orig_termios;

void reset_terminal_mode() {
    tcsetattr(0, TCSAFLUSH, &orig_termios);
}

void set_terminal_raw_mode() {
    struct termios raw;
    if (!isatty(0)) {
        fprintf(stderr, "Not a terminal.\n");
        return;
    }
    tcgetattr(0, &orig_termios);
    atexit(reset_terminal_mode);
    
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1; // return at 1 char
    raw.c_cc[VTIME] = 0; // no timeout
    tcsetattr(0, TCSAFLUSH, &raw);
}

void* keyboard_thread_func(void* arg) {
    struct mvvm *vm = arg;
    set_terminal_raw_mode();
    int ch;
    while ((ch = getchar()) != EOF) {
        write_to_serial(vm, (char)ch);
    }
    return NULL;
}

#define MEM_SIZE (1024*1024*1024)
#define KERNEL_ARGS "console=ttyS0 debug"

int main(int argc, char *argv[]) {
    struct mvvm vm;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <kernel> <initrd>\n", argv[0]);
        return -1;
    }
    if (mvvm_init(&vm, MEM_SIZE) < 0) {
        return -1;
    }
    if (mvvm_load_kernel(&vm, argv[1], argv[2], KERNEL_ARGS) < 0) {
        return -1;
    }
    pthread_t keyboard_thread;
    if (pthread_create(&keyboard_thread, NULL, keyboard_thread_func, &vm) != 0) {
        perror("Failed to create thread");
        return 1;
    }
    int ret;
    if ((ret = mvvm_run(&vm)) != 0) {
        fprintf(stderr, "mvvm exit code: %d\n", ret);
        return ret;
    }
    mvvm_destroy(&vm);
    return 0;
}