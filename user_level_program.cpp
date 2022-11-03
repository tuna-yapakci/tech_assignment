#include <iostream>
#include <thread>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define MAGIC 'k'
#define USER_APP_REG _IOW(MAGIC, 1, int)

void reader_func() {
    std::cout << "reader thread initialized" << std:: endl;
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

    int file = open("/dev/custom_gpio_dev", O_RDWR);
    if (file < 0) {
        std::cout << "Could't open file " << std::endl;
    }

    //first register the process to the driver
    pid_t pid = getpid();
    std::cout << "Process ID is " << pid << std::endl;
    if(ioctl(file, USER_APP_REG, (int) pid)) {
        std::cout << "Couldn't register to driver" << std::endl;
        close(file);
        exit(EXIT_FAILURE);
    };
    
    //reader.join();
    //std::cout << "reader thread joined" << std::endl;
}

