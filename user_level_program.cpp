#include <iostream>
#include <thread>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <csignal>

#define MAGIC 'k'
#define USER_APP_REG _IOW(MAGIC, 1, int*)
#define START_COMMS _IO(MAGIC, 2)
#define SIGDATARECV 47

void reader_func() {
    std::cout << "reader thread initialized" << std:: endl;
}

void signal_handler(int sig_num) {
    std::cout << "Signal received! " << sig_num << std:: endl;

    int file = open("/dev/custom_gpio_dev", O_RDWR);
    if (file < 0) {
        std::cout << "Could't open file " << std::endl;
    }
    ioctl(file, START_COMMS);
}

int main() {
    //initialize reader thread
    //std::thread reader(reader_func);
    /*
    char message[10];
    std::cout << "Maximum message length is 10 characters" << std::endl;
    std::cout << "Type your message: " << std::endl;
    fgets(message, 10, stdin);
    std::cout << "Message is " << message << std::endl;
    */

    //registering signal
    signal(SIGDATARECV, signal_handler);

    int file = open("/dev/custom_gpio_dev", O_RDWR);
    if (file < 0) {
        std::cout << "Could't open file " << std::endl;
    }

    //first register the process to the driver
    pid_t pid = getpid();
    std::cout << "Process ID is " << pid << std::endl;
    if(ioctl(file, USER_APP_REG, (int*) &pid)) {
        std::cout << "Couldn't register to driver" << std::endl;
        close(file);
        exit(EXIT_FAILURE);
    };


    while(1) {  
        std::cout << "Doing code stuff..." << std::endl;
        sleep(1);
    }
    
    //reader.join();
    //std::cout << "reader thread joined" << std::endl;
    return 0;
}

