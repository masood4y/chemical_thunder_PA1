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
#include <time.h>
     


// TODO: ensure all static variables are updated appropriately
_local static unsigned int sender_current_state;
_local static unsigned long long int bytes_left_to_send;
_local static FILE *file_pointer;
// Socket

_local static uint16_t acknowledged[2];
_local static uint16_t in_Flight[2];
extern volatile static uint16_t current_window_size;
extern volatile static double RTT_in_ms;

_local const static uint16_t max_window_size = 21845; // max uint16_t / 3

_local static clock_t start, end;
_local static double cpu_time_used_in_seconds;
_local static double cpu_time_used_in_ms;
     

struct protocol_Header
{





}


struct protocol_Packet
{

    



}


enum sender_state
{
    /* Connection Setup */
    Start_Connection,

    /* Send Data*/
    Send_N_Packets,
    Wait_for_Ack,

    /* Connection Teardown */
    Send_Fin,
    Wait_Fin_Ack
};


/* Initialization */
_local void sender_init(void);
_local bool open_file(char* filename, unsigned long long int bytesToTransfer);

// close file
_local void sender_finish(void);

/* Connection Setup */
_local void sender_action_Start_Connection(void);

/* Send Data*/
_local void sender_action_Send_N_Packets(void);
_local void sender_action_Wait_for_Ack(void);

/* Connection Teardown */
_local void sender_action_Send_Fin(void);
_local void sender_action_Wait_Fin_Ack(void);



/* Initialization */
_local bool sender_init(char* filename, unsigned long long int bytesToTransfer,
                        char* hostname, unsigned short int hostUDPport)
{

    // set up udp socket for listening and sending

    // set up statemachine





    /* File related initialization */
    if (!open_file(filename, bytesToTransfer))
    {
        return false;
    }

    // // open udp port to hostname for sending
    // // Define the hostname and port

    // // Resolve the hostname to an IP address
    // struct hostent *host = gethostbyname(hostname);
    // if (host == NULL) {
    //     fprintf(stderr, "Error: Could not resolve hostname.\n");
    //     return false;
    // }

    // // Create a UDP socket
    // int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    // if (sockfd < 0) {
    //     perror("Error opening socket");
    //     return false;
    // }

    // // Set up the server address structure
    // struct sockaddr_in server_addr;
    // memset(&server_addr, 0, sizeof(server_addr));
    // server_addr.sin_family = AF_INET;
    // server_addr.sin_port = htons(hostUDPport); 
    // memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);

    // // // Example: Sending data (you can replace this with your own data sending logic)
    // // const char *message = "Hello, UDP server!";
    // // int message_len = strlen(message);

    // // // Send data to the server
    // // ssize_t bytes_sent = sendto(sockfd, message, message_len, 0,
    // //                              (struct sockaddr *)&server_addr, sizeof(server_addr));
    // // if (bytes_sent < 0) {
    // //     perror("Error sending data");
    // //     close(sockfd);
    // //     exit(1);
    // // }

    // // printf("Sent %zd bytes to %s:%s\n", bytes_sent, hostname, portname);

    // // Listening for response
    // struct sockaddr_in client_addr;
    // socklen_t client_len = sizeof(client_addr);
    // char buffer[MAX_BUFFER_SIZE];
    // ssize_t bytes_received = recvfrom(sockfd, buffer, MAX_BUFFER_SIZE, 0,
    //                                   (struct sockaddr *)&client_addr, &client_len);
    // if (bytes_received < 0) {
    //     perror("Error receiving data");
    //     close(sockfd);
    //     exit(1);
    // }

    // printf("Received %zd bytes from %s:%d\n", bytes_received,
    //        inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    // printf("Message received: %s\n", buffer);

    // // Close the socket
    // close(sockfd);








    // set up state machine
    sender_current_state = Start_Connection;
    return true;
}

_local bool open_file(char* filename, unsigned long long int bytesToTransfer) 
{
    file_pointer = fopen(filename, "r");
    if (file_pointer == NULL) {
        fprintf(stderr, "Error: Could not open filename.\n");
        return false;
    }

    
    // get file size
    long long file_size;
    bytes_left_to_send = bytesToTransfer;
    fseek(file_pointer, 0, SEEK_END);
    file_size = ftell(file_pointer);
    fseek(file_pointer, 0, SEEK_SET);
    
    // set bytes_left_to_send as MIN(bytesToTransfer, Filesize)
    if (file_size < bytesToTransfer) {
        bytes_left_to_send = file_size;
    }

    return true;
}








/* Connection Setup */
_local void sender_action_Start_Connection(void)
{

    // send SYNC = 1 to dest
    
    // start timer
        // what should timer be? 2 seconds


    start = clock();
    // wait for 2 seconds
    while(1)
    {
        end = clock();
        cpu_time_used_in_seconds = ((double) (end - start)) / CLOCKS_PER_SEC;
        
        // check socket for SYNC ACK = 1 
        if (SYNC_ACK = 1 received)
        {
            // set current window size using cwnd stuff
            sender_current_state = Send_N_Packets;
            break;
        }

        // if (timer end), go to Start Connection State.
        else if (cpu_time_used_in_seconds >= 2)
        {
            break;
        }

        else if (SYNC_ACK = 1 received)
        {
            // TODO: set current window size using cwnd stuff
            sender_current_state = Send_N_Packets;
            break;
        }
    }
    return;
}






/* Send Data*/
_local void sender_action_Send_N_Packets(void) 
{
    //TODO: fix sliding window
    // send first packet in cwnd
        // 
    // start timer
    start = clock();

    // send rest of the packets in cwnd

    sender_current_state = Wait_for_Ack;

    return;
}
_local void sender_action_Wait_for_Ack(void)
{

    while(1){
        end = clock();
        cpu_time_used_in_ms = ((double) (end - start)) / (CLOCKS_PER_SEC / 1000 );
        // check port for Packet

        if (ACK_Valid_received) {
            // update bytes left
            // if bytes left == 0, goto: Send_FIN
            // adjust sliding window, update current window size
            // goto: Send N Packets
        }

        else if 



    }


    return;

}


void rsend(char* hostname, 
            unsigned short int hostUDPport, 
            char* filename, 
            unsigned long long int bytesToTransfer) 
{
    if (!sender_init())
    {   
        return error;
    }

    




    while (1) {
        // loop over states and call their functions
    }

    sender_finish();
}

int main(int argc, char** argv) {
    // This is a skeleton of a main function.
    // You should implement this function more completely
    // so that one can invoke the file transfer from the
    // command line.
    int hostUDPport;
    unsigned long long int bytesToTransfer;
    char* hostname = NULL;
    char* filename = NULL;

    if (argc != 5) {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    hostUDPport = (unsigned short int) atoi(argv[2]);
    hostname = argv[1];
    filename = argv[3];
    bytesToTransfer = atoll(argv[4]);

    rsend(hostname, hostUDPport, filename, bytesToTransfer);

    return (EXIT_SUCCESS);
}