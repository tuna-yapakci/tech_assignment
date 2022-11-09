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
#define MASTERNAME "/dev/gpio_master"
#define SLAVENAME "/dev/gpio_slave"

static const char* dev_file;

//prototype
int send_message(std::string *msg, int is_command);

void signal_handler(int sig_num) {
    //std::cout << "Signal received: " << sig_num << std:: endl;
    int file = open(dev_file, O_RDWR);
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
        //std::cout << "Data received" << std::endl;
        char str[11];
        read(file, &str, 11);
        u_int8_t l = (u_int8_t) str[0];
        int len = (int) l;
        str[len + 1] = '\0';
        if (str[1] == 0xBB) {
            std::cout << "The other side commands: " << &(str[2]) << std::endl;
            std::string msg;
            msg.push_back(0xBC);
            for (int i = 0; i < len - 1; i += 1) {
                msg.push_back(str[i+2] + 2);
            }
            if(send_message(&msg, 0) < 0) {
                std::cout << "Error responding to command (queue might be full)" << std::endl;
            }
            else {
                std::cout << "Replied to command, length = " << msg.length() << std::endl;
            }
        }
        else if (str[1] == 0xBC) {
            std::cout << "The other side replied: " << &(str[2]) << std::endl;
        }
        else {
            std::cout << "The other side says: " << &(str[1]) << std::endl;
        }
    }
    close(file);
}

int send_message(std::string *msg, int is_command){
    int file = open(dev_file, O_RDWR);
    if (file < 0) {
        std::cout << "Could't open dev file " << std::endl;
        return -1;
    }

    if(is_command) {
        char message[msg->length() + 1];
        message[0] = 0xBB;
        for (unsigned int i = 1; i < msg->length() + 1; i += 1) {
            message[i] = msg->at(i-1);
        }
        if(write(file, message, msg->length() + 1) < 0) {
            close(file);
            return -1;
        }
    }
    else {
        char message[msg->length()];
        for (unsigned int i = 0; i < msg->length(); i += 1) {
            message[i] = msg->at(i);
        }
        if(write(file, message, msg->length()) < 0) {
            close(file);
            return -1;
        }
    }

    close(file);
    return 0;
}

int main(int argc, char *argv[]) {

    if(argc != 2) {
        std::cout << "Usage: ./user_app [driver_mode]" << std::endl;
        std::cout << "driver_mode is 0 for master, 1 for slave" << std::endl;
        std::cout << "(This is needed as we simulate both devices in the same computer)" << std::endl;
        exit(EXIT_FAILURE);
    }

    if(argv[1][0] == '0') { //this just checks the first char of the argument
        dev_file = MASTERNAME;
    }
    else if(argv[1][0] == '1') {
        dev_file = SLAVENAME;
    }
    else {
        std::cout << "Invalid mode entered" << std::endl;
        exit(EXIT_FAILURE);
    }

    //registering signal
    signal(SIGDATARECV, signal_handler);
    signal(SIGINT, signal_handler);

    int file = open(dev_file, O_RDWR);
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
        std::string mode;
        std::string msg;
        std::cout << "Enter \"m\" to send a message, \"c\" to send a command:" << std::endl;
        std::cin >> mode;
        if(mode.length() == 1 && mode[0] == 'm') {
            std::cout << "Maximum message length is " << MAX_NUM_BYTES_IN_A_MESSAGE << " bytes" << std::endl;
            std::cout << "Enter your message: " << std::endl;
            std::cin.ignore();
            getline(std::cin, msg);
            if (msg.length() == 0 || msg.length() > MAX_NUM_BYTES_IN_A_MESSAGE) {
                std::cout << "Invalid message length (" << msg.length() << ")" << std::endl;
            }
            else {
                if(send_message(&msg, 0) < 0){
                    std::cout << "Error sending message (queue might be full)" << std::endl;
                }
                else {
                    std::cout << "Message sent, length = " << msg.length() << std::endl;
                }
            }
        }
        else if (mode.length() == 1 && mode[0] == 'c') {
            std::cout << "Maximum command length is " << (MAX_NUM_BYTES_IN_A_MESSAGE - 1) << " bytes" << std::endl;
            std::cout << "Enter your command: " << std::endl;
            std::cin.ignore();
            getline(std::cin, msg);
            if (msg.length() == 0 || msg.length() > (MAX_NUM_BYTES_IN_A_MESSAGE - 1)) {
                std::cout << "Invalid command length (" << msg.length() << ")" << std::endl;
            }
            else {
                if(send_message(&msg, 1) < 0) {
                    std::cout << "Error sending command (queue might be full)" << std::endl;
                }
                else {
                    std::cout << "Command issued, length = " << msg.length() << std::endl;
                }
            }
        }
        else {
            std::cout << "Invalid mode, enter either \"m\" or \"c\"" << std::endl;
        }
    }

    return 0;
}