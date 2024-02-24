#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>
#include <errno.h>

//#define all the defines here

_local static unsigned int receiver_current_state;

_local static unsigned long long int receiver_write_rate;
_local static FILE *receiver_file;


// enum receiver_event
// {
//     /* Connection Setup */
//     Wait_Connection,

//     /* Receive Data*/
//     SYNC_RECEIVED,
//     DUP

//     /* Connection Teardown */
//     LONG_TIMER_FINISH,
//     FIN_RECEIVED
// };

enum receiver_state
{
    /* Connection Setup */
    Wait_Connection,

    /* Receive Data*/
    Wait_for_Packet,
    Wait_for_Pipleline,

    /* Connection Teardown */
    Send_Fin_Ack,
    Wait_inCase
};

// Initialization.
_local bool receiver_init(void);
// Closing file, socket, etc.
_local void receiver_finish(void);

/* Connection Setup */
_local void receiver_action_Wait_Connection(void);

/* Receive Data*/
_local void receiver_action_Wait_for_Packet(void);
_local void receiver_action_Wait_for_Pipeline(void);

/* Connection Teardown */
_local void receiver_action_Send_Fin_Ack(void);
_local void receiver_action_Wait_inCase(void);





void rrecv(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) {

    if (!receiver_init(myUDPport, destinationFile, writeRate)) {
        // Error'd out somewhere in initialization.
        receiver_finish();
        return;
    }

    // call initialize
    // run in a while(1) with switch on states

    while(true) {
        switch(receiver_current_state) {
            case(Wait_Connection):
                receiver_action_Wait_Connection();
                break;
            case(Wait_for_Packet):
                receiver_action_Wait_for_Packet();
                break;
            case(Wait_for_Pipleline):
                receiver_action_Wait_for_Pipeline();
                break;
            case(Send_Fin_Ack):
                receiver_action_Send_Fin_Ack();
                break;
            case(Wait_inCase):
                receiver_action_Wait_inCase();
                break;
        }
    }

}

bool receiver_init(unsigned short int myUDPport, 
            char* destinationFile, 
            unsigned long long int writeRate) {
    // Create the UDP socket.
    socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket < 0) {
        perror("Error with creating socket.");
        return false;
    }
    
    // Set the socket as non-blocking.
    if (fcntl(socket, FSETFL, fcntl(socket, FGETFL, 0) | O_NONBLOCK)) {
        perror("Error with setting socket flags.");
        return false;
    }

    // Set socket address for receiving.
    struct sockaddr_in receiver_socket_addr;
    receiver_socket_addr.sin_family = AF_INET;
    receiver_socket_addr.sin_port = htons(myUDPport);
    receiver_socket_addr.sin_addr = INADDR_ANY; // no-specific IP-address

    // Bind the socket to the address and port.
    if (bind(socket, (struct sockaddr *)receiver_socket_addr, sizeof(receiver_socket_addr)) < 0) {
        perror("Error binding to the port.");
        return false;
    }

    // Open file for writing.
    FILE *filePointer;
    filePointer = fopen(destinationFile, "w");

    if (filePointer == NULL) {
        perror("Error opening file.");
        return false;
    }

    // Set static variables.
    receiver_file = filePointer;
    receiver_write_rate = writeRate;
    
    // Set default values.
    receiver_current_state = Wait_Connection;
}

void receiver_action_Wait_Connection(void) {
    // Wait for packet...
    // Listen to port
    if (SYNC = 1 received) {
        // set up UDP port, 
        // set up Receive Window,
        // send SYNC_ACK = 1 and writeRate back to destination, 
        receiver_current_state = Wait_for_Packet;
    } else {
        // Otherwise we do nothing and just wait.
        receiver_current_state = Wait_Connection;
    }
}

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.

    unsigned short int udpPort;
    char* filename = NULL;
    unsinged long long int writeRate = 0; // FIXME: this is just default (no limit), will need to be fixed later on.

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);
    filename = argv[2];

    rrecv(udpPort, filename, writeRate);
}
