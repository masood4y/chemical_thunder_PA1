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
#include "our_protocol.h"
     


// TODO: ensure all static variables are updated appropriately
_local static unsigned int sender_current_state;
_local static unsigned long long int bytes_left_to_send;
_local static FILE *file_pointer;
_local static int sockfd;

_local static uint16_t acknowledged[2];
_local static uint16_t in_Flight[2];
extern volatile static uint16_t current_window_size;
extern volatile static double RTT_in_ms;


_local const static uint16_t max_window_size = 21750; /* Set as (uint16_t / 3) */ 

_local static clock_t start, end;
_local static double cpu_time_used_in_seconds;
_local static double cpu_time_used_in_ms;
static long long int file_offset_for_sending;
     

enum sender_state
{
    /* Connection Setup */
    Start_Connection,

    /* Send Data*/
    Send_N_Packets,
    Wait_for_Ack,

    /* Connection Teardown */
    Send_Fin,
    Wait_Fin_Ack,
    sender_Done
};


/* Initialization */
_local void sender_init(void);
_local bool open_file(char* filename, unsigned long long int bytesToTransfer);

//TODO: close file, close socket
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

    /* File related initialization */
    if (!open_file(filename, bytesToTransfer))
    {
        return false;
    }


    /* Socket Set up for Listening and Sending to hostname */
    if (!setup_socket(hostname, hostUDPport))
    {
        return false;
    }

    current_window_size = 1450;
    if (bytes_left_to_send < current_window_size){
            current_window_size = bytes_left_to_send;
    }
    acknowledged[2];
    in_Flight[0] = 0;
    in_Flight[1] = in_Flight[0] + current_window_size;
    acknowledged[0] = in_Flight[1] + 1;
    acknowledged[1] = in_Flight[0] - 1;

    // // Main loop for bidirectional communication
    // while (1) {
    //     // Receive data (non-blocking)
    //     ssize_t bytes_received = recv(sockfd, buffer, MAX_BUFFER_SIZE, MSG_DONTWAIT);
    //     if (bytes_received > 0) {
    //         printf("Received %zd bytes\n", bytes_received);
    //         printf("Message received: %s\n", buffer);
    //     } else if (bytes_received == 0) {
    //         printf("Connection closed by peer\n");
    //         break;
    //     } else if (bytes_received == -1) {
    //         // Check if the error is due to the socket being non-blocking
    //         if (errno != EAGAIN && errno != EWOULDBLOCK) {
    //             perror("Error receiving data");
    //             close(sockfd);
    //             exit(EXIT_FAILURE);
    //         }
    //     }

    //     // Send data
    //     const char *message = "Hello, UDP server!";
    //     int message_len = strlen(message);
    //     ssize_t bytes_sent = send(sockfd, message, message_len, 0);
    //     if (bytes_sent < 0) {
    //         perror("Error sending data");
    //         close(sockfd);
    //         exit(EXIT_FAILURE);
    //     }

    //     printf("Sent %zd bytes\n", bytes_sent);

    //     // Sleep for a short interval to prevent busy-waiting
    //     usleep(100000); // 100 ms
    // }

    // // Close the socket (not reached in this example)
    // close(sockfd);



    //TODO: set up sliding window
        // acknowledged[];
        // acknowledged[2];
        // in_Flight[2];
   



    /* Set up State machine */
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
    
    /* Get File Size */
    long long file_size;
    
    fseek(file_pointer, 0, SEEK_END);
    file_size = ftell(file_pointer);
    fseek(file_pointer, 0, SEEK_SET);
    file_offset_for_sending = 0;
    
    /* Set bytes_left_to_send as MIN(bytesToTransfer, Filesize) */
    if (file_size < bytesToTransfer) {
        bytes_left_to_send = file_size;
    }
    else {
        bytes_left_to_send = bytesToTransfer;
    }
    return true;
}
_local bool setup_socket(char* hostname, unsigned short int hostUDPport) {

    struct sockaddr_in receiver_address;
    struct hostent *host;
    sockfd = -1;

    /* Resolve the hostname */
    host = gethostbyname(hostname);
    if (host == NULL) {
        fprintf(stderr, "Error: Could not resolve hostname.\n");
        return false;
        //exit(EXIT_FAILURE);
    }

    /* Create a UDP socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error opening socket");
        return false;
        //exit(EXIT_FAILURE);
    }

    //TODO: bind the socket?

    /* Set up the receiver struct */
    memset(&receiver_address, 0, sizeof(receiver_address));
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_port = htons(hostUDPport);
    memcpy(&receiver_address.sin_addr, host->h_addr, host->h_length);

    /* Connect other end of socket to the Receiver */ 
    if (connect(sockfd, (struct sockaddr *)&receiver_address, sizeof(receiver_address)) < 0) {
        perror("Error connecting to server");
        close(sockfd);
        return false;
        // exit(EXIT_FAILURE);
    }

    //printf("UDP socket connected to %s:%s\n", HOSTNAME, PORT);

    /* Set the socket to non-blocking mode for receiving */
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("Error getting socket flags");
        return false;
        //close(sockfd);
        //exit(EXIT_FAILURE);
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("Error setting socket to non-blocking mode");
        return false;
        //close(sockfd);
        //exit(EXIT_FAILURE);
    }


    return true;


}







/* Connection Setup */
_local void sender_action_Start_Connection(void)
{
    /* send SYNC = 1 to receiver */ 
    struct protocol_Header sync_header;

    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    sync_header.management_byte = 0;
    sync_header.seq_ack_num = 0;
    sync_header.management_byte = management_byte | 0x80;

    ssize_t bytes_sent = send(sockfd, &sync_header, sizeof(sync_header), 0);

    if (bytes_sent < 0) {
        perror("Error sending data");
        //TODO: handle
    }
    
    /* Start 2 second timer */
    start = clock();
    while(1)
    {
        end = clock();
        cpu_time_used_in_seconds = ((double) (end - start)) / CLOCKS_PER_SEC;

        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, 512, MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Sync Ack*/
            if ((receive_buffer.management_byte & 0x40) == 0x40) {
                //TODO: set up sliding window, current packet size, RTT?
                sender_current_state = Send_N_Packets;
                break;
            }
        } 
        else if (bytes_received == 0) 
        {
            printf("Connection closed by peer\n");
            //TODO: handle
            //break;
        } 
        /* Check if the error is due to the socket being non-blocking */
        else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            perror("Error receiving data");
            //close(sockfd);
            //exit(EXIT_FAILURE);
            // TODO: handle
        }

        /* Check Timer for timeout */
        else if (cpu_time_used_in_seconds >= 2)
        {
            break;
        }
    }
    return;    
}


/* Send Data*/
_local void sender_action_Send_N_Packets(void) 
{
    uint16_t sending_index;
    struct protocol_Packet packet_being_sent;
    sending_index = in_Flight[0];
    bool first_packet = true;
    
    if (in_Flight[1] > in_Flight[0])
    {
        while(sending_index <= in_Flight[1])
        {
            int i;
            packet_being_sent.header.management_byte = 0;
            for (i = 0; i < 1450, sending_index <= in_Flight[1]; i++)
            {
                protocol_Packet.data[i] = fgetc(file_pointer);
                sending_index++; 
            }
            packet_being_sent.header.management_byte = 0;
            packet_being_sent.header.seq_ack_num = sending_index - 1;
            if (i != 1450) {
                for (int j = i; j < 1450; j++){
                    protocol_Packet.data[j] = EOF;
                }
            }
            ssize_t bytes_sent = send(sockfd, &packet_being_sent, sizeof(protocol_Packet), 0);
            
            // TODO: error checking on send
            if (first_packet)
            {
                start = clock();
                first_packet = false;
            }
        }
    }
    else if (in_Flight[0] > in_Flight[1])
    {
        while(sending_index <= in_Flight[1] || sending_index >= in_Flight[0])
        {
            int i;
            packet_being_sent.header.management_byte = 0;
            for (i = 0; i < 1450, (sending_index <= in_Flight[1] || sending_index >= in_Flight[0]); i++)
            {
                protocol_Packet.data[i] = fgetc(file_pointer);
                sending_index++; 
            }
            packet_being_sent.header.management_byte = 0;
            packet_being_sent.header.seq_ack_num = sending_index - 1;
            if (i != 1450) {
                for (int j = i; j < 1450; j++){
                    protocol_Packet.data[j] = EOF;
                }
            }
            ssize_t bytes_sent = send(sockfd, &packet_being_sent, sizeof(protocol_Packet), 0);
            // TODO: error checking on send
            if (first_packet)
            {
                start = clock();
                first_packet = false;
            }
        }
    }
    sender_current_state = Wait_for_Ack;
    return;
}

_local void sender_action_Wait_for_Ack(void)
{
    if (bytes_left_to_send == 0) {
        sender_current_state = Send_Fin;
        return;
    }

    while(1)
    {
        end = clock();
        cpu_time_used_in_ms = ((double) (end - start)) / (CLOCKS_PER_SEC / 1000 );
        

        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, 512, MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Valid Seq number */
            uint16_t ack_num = receive_buffer.seq_ack_num;
            if (valid_ack_num(ack_num)) 
            {
                uint16_t old_acked = acknowledged[1];
                acknowledged[1] = ack_num - 1;
                bytes_left_to_send = bytes_left_to_send - (acknowledged[1] - old_acked);
                in_Flight[0] = ack_mum;
                if (bytes_left_to_send == 0){
                    sender_current_state = Send_Fin;
                    break;
                }
                // update bytes left, if bytes left to send == 0, goto Send_FIN

                // update file offset for sending
                file_offset_for_sending =+ (acknowledged[1] - old_acked);
                fseek(file_pointer, file_offset_for_sending, SEEK_SET);
                
                //TODO: update current window size based on bytes left, AMID, theoretical max
                if (current_window_size < max_window_size) {
                    current_window_size = current_window_size + 1450;
                }
                if (bytes_left_to_send < current_window_size){
                    current_window_size = bytes_left_to_send;
                }
                bytes_left_to_send = bytes_left_to_send - (acknowledged[1] - old_acked);
                in_Flight[1] = in_Flight[0] + current_window_size;
                acknowledged[0] = in_Flight[1] + 1;
                sender_current_state = Send_N_Packets;
                break;
            }
        } 
        else if (bytes_received == 0) 
        {
            printf("Connection closed by peer\n");
            //TODO: handle
            //break;
        } 
        /* Check if the error is due to the socket being non-blocking */
        else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            perror("Error receiving data");
            //close(sockfd);
            //exit(EXIT_FAILURE);
            // TODO: handle
        }
        else if(cpu_time_used_in_ms > 2000) //TODO: figure out time to use
        {
            //TODO: update current window size based on bytes left, AMID, theoretical max
                //in_Flight[1];
                //acknowledged[0];
                // go to Send N Packets
            current_window_size =/2;
            current_window_size + 1450 - (current_window_size % 1450);
            in_Flight[1] = in_Flight[0] + current_window_size;
            acknowledged[0] = in_Flight[1] + 1;
            fseek(file_pointer, file_offset_for_sending, SEEK_SET);
            sender_current_state = Send_N_Packets;
            break;
        }

        

        // if its a valid ack
            // update bytes left, if bytes left to send == 0, goto Send_FIN
            // update file offset for sending
            // update current window size based on bytes left, AMID, theoretical max
            // go to Send N Packets
        

        //else if timeout
            // update file offset for sending
            // update current window size AMID, theoretical max
            // go to Send N Packets
    
    }
    return;

}

_local bool valid_ack_num(uint16_t ack_num) 
{

    if (in_Flight[0] < in_Flight[1]){
        return ((ack_num >= in_Flight[0]) && (ack_num <= (in_Flight[1] + 1)));
    }

    else if (in_Flight[0] > in_Flight[1])
    {   
        return (ack_num >= in_Flight[0]) || (ack_num <= (in_Flight[1] + 1));
    }

}



/* Connection Teardown */
_local void sender_action_Send_Fin(void)
{
    /* send FIN = 1 to receiver */ 
    struct protocol_Header sync_header;

    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    sync_header.management_byte = 0;
    sync_header.seq_ack_num = 0;
    sync_header.management_byte = management_byte | 0x02;

    ssize_t bytes_sent = send(sockfd, &sync_header, sizeof(sync_header), 0);

    if (bytes_sent < 0) {
        perror("Error sending data");
        //TODO: handle
    }
    start = clock();

    sender_current_state = Wait_Fin_Ack;
    return;
}


_local void sender_action_Wait_Fin_Ack(void)
{
    // Wait_FIN_Ack: do nothing/wait
    //         if (timeout), goto: Send_FIN
    //         else if (FIN_ACK = 1 received), done 
    
    while(1)
    {
        end = clock();
        cpu_time_used_in_seconds = ((double) (end - start)) / CLOCKS_PER_SEC;

        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, 512, MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Fin Ack*/
            if ((receive_buffer.management_byte & 0x2) == 0x2) {
                //TODO: set up sliding window, current packet size, RTT?
                sender_current_state = sender_Done;
                break;
            }
        } 
        else if (bytes_received == 0) 
        {
            printf("Connection closed by peer\n");
            //TODO: handle
            //break;
        } 
        /* Check if the error is due to the socket being non-blocking */
        else if ((bytes_received == -1) && (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            perror("Error receiving data");
            //close(sockfd);
            //exit(EXIT_FAILURE);
            // TODO: handle
        }

        /* Check Timer for timeout */ //TODO: fix timer
        else if (cpu_time_used_in_seconds >= 2)
        {
            sender_current_state = Send_Fin;
            break;
        }
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
        sender_finish();
        return error;
    }

    while (sender_current_state != sender_Done) 
    {
        //TODO: figure out how to break from while(1) loop at the end
        switch (sender_current_state) 
        {
            /* Connection Setup */
            case Start_Connection:
                sender_action_Start_Connection();
                break;


            /* Send Data*/
            case Send_N_Packets:
                sender_action_Send_N_Packets();
                break;

            case Wait_for_Ack:
                sender_action_Wait_for_Ack();
                break;


            /* Connection Teardown */
            case Send_Fin:
                sender_action_Send_Fin();
                break;

            case Wait_Fin_Ack:
                sender_action_Wait_Fin_Ack();
                break;

            default: 
        }    
    }
    sender_finish();
}

int main(int argc, char** argv) {

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