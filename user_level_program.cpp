#include <iostream>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <csignal>

#define MAGIC 'k'
#define USER_APP_REG _IOW(MAGIC, 1, int*)
#define USER_APP_UNREG _IO(MAGIC, 2)
#define SIGDATARECV 47
#define MAX_NUM_BYTES_IN_A_MESSAGE 10

void signal_handler(int sig_num) {
    std::cout << "Signal received: " << sig_num << std:: endl;
    int file = open("/dev/custom_gpio_dev", O_RDWR);
    if (file < 0) {
        std::cout << "Could't open file" << std::endl;
        return;
    }
    if (sig_num == SIGINT) {
        std::cout << "Signaling kernel and terminating app"<< std::endl;
        ioctl(file, USER_APP_UNREG);
        exit(EXIT_SUCCESS);
    }
    else if (sig_num == SIGDATARECV) {
        std::cout << "Data received" << std::endl;
    }
    close(file);
}

int send_message(std::string *msg, int is_command){
    int file = open("/dev/custom_gpio_dev", O_RDWR);
    if (file < 0) {
        std::cout << "Could't open file " << std::endl;
        return -1;
    }

    if(is_command) {
        char message[msg->length() + 1];
        message[0] = 0xBB;
        for (int i = 1; i < msg->length() + 1; i += 1) {
            message[i] = msg->at(i);
        }
        if(write(file, message, msg->length() + 1) < 0) {
            return -1;
        }
    }
    else {
        char message[msg->length()];
        for (int i = 0; i < msg->length(); i += 1) {
            message[i] = msg->at(i);
        }
        if(write(file, message, msg->length())) {
            return -1;
        }
    }

    close(file);
    return 0;
}

int main() {
    //registering signal
    signal(SIGDATARECV, signal_handler);
    signal(SIGINT, signal_handler);

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

    /*
    while(1) {  
        std::cout << "Doing code stuff..." << std::endl;
        sleep(1);
    }
    */

    while(1) {
        std::string mode;
        std::string msg;
        std::cout << "Enter \"m\" to send a message, \"c\" to send a command:" << std::endl;
        std::cin >> mode;
        if(mode.length() == 1 && mode[0] == 'm') {
            std::cout << "Maximum message length is " << MAX_NUM_BYTES_IN_A_MESSAGE << std::endl;
            std::cout << "Enter your message: " << std::endl;
            getline(std::cin, msg);
            if (msg.length() == 0 || msg.length() > MAX_NUM_BYTES_IN_A_MESSAGE) {
                std::cout << "Invalid message length" << std::endl;
            }
            else {
                if(send_message(&msg, 0) < 0){
                    std::cout << "Error sending message (queue might be full)" << std::endl;
                }
                std::cout << "Message sent" << std::endl;
            }
        }
        else if (mode.length() == 1 && mode[0] == 'c') {
            std::cout << "Maximum command length is " << (MAX_NUM_BYTES_IN_A_MESSAGE - 1) << std::endl;
            std::cout << "Enter your command: " << std::endl;
            getline(std::cin, msg);
            if (msg.length() == 0 || msg.length() > (MAX_NUM_BYTES_IN_A_MESSAGE - 1)) {
                std::cout << "Invalid command length" << std::endl;
            }
            else {
                if(send_message(&msg, 1) < 0) {
                    std::cout << "Error sending command (queue might be full)" << std::endl;
                }
                std::cout << "Command issued" << std::endl;
            }
        }
        else {
            std::cout << "Invalid mode" << std::endl;
        }
    }

    return 0;
}

// also send a signal to the driver for when the app is closed.