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

// 
_local void receiver_init(void);

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

    receiver_init();

    // call initialize
    // run in a while(1) with switch on states



}

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);
}
