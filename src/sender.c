#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
//TODO: change?
#include <netdb.h> // Add this line

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include "our_protocol.h"


// TODO: ensure all static variables are updated appropriately
#define ALPHA 0.125
#define BETA 0.25

static unsigned int sender_current_state;
static unsigned long long int bytes_left_to_send;
static FILE *file_pointer;
static int sockfd;

static uint16_t acknowledged[2];
static uint16_t in_Flight[2];
static uint16_t current_window_size;
static double RTT_in_ms;
static double timeoutInterval_in_ms;
static double devRTT;


static clock_t start, end;
static double cpu_time_used_in_seconds;
static double cpu_time_used_in_ms;
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
int sender_init(char* filename, unsigned long long int bytesToTransfer,
                        char* hostname, unsigned short int hostUDPport);
int open_file(char* filename, unsigned long long int bytesToTransfer); 
int setup_socket(char* hostname, unsigned short int hostUDPport);
void updateRTT(double sampleRTT);

//TODO: close file, close socket
void sender_finish(void);

/* Connection Setup */
void sender_action_Start_Connection(void);

/* Send Data*/
void sender_action_Send_N_Packets(void);
void sender_action_Wait_for_Ack(void);
int valid_ack_num(uint16_t ack_num);

/* Connection Teardown */
void sender_action_Send_Fin(void);
void sender_action_Wait_Fin_Ack(void);



/* Initialization */
int sender_init(char* filename, unsigned long long int bytesToTransfer,
                        char* hostname, unsigned short int hostUDPport)
{
    sockfd = -1;
    /* File related initialization */
    if (open_file(filename, bytesToTransfer))
    {   
        printf("Could not open file\n");
        return -1;
    }


    /* Socket Set up for Listening and Sending to hostname */
    if (setup_socket(hostname, hostUDPport))
    {
        printf("Could not setup_socket\n");
        return -1;
    }

    current_window_size = PROTOCOL_DATA_SIZE;
    if (bytes_left_to_send < current_window_size){
        current_window_size = bytes_left_to_send;
    }
    in_Flight[0] = 0;
    in_Flight[1] = in_Flight[0] + (current_window_size - 1);
    acknowledged[0] = in_Flight[1] + 1;
    acknowledged[1] = in_Flight[0] - 1;


    /* Set up State machine */
    sender_current_state = Start_Connection;
    return 0;
}

int open_file(char* filename, unsigned long long int bytesToTransfer) 
{
    file_pointer = fopen(filename, "r");
    if (file_pointer == NULL) {
        fprintf(stderr, "Error: Could not open filename.\n");
        return -1;
    }
    
    /* Get File Size */
    long long file_size;
    
    fseek(file_pointer, 0, SEEK_END);
    file_size = ftell(file_pointer);
    fseek(file_pointer, 0, SEEK_SET);
    file_offset_for_sending = 0;
    
    /* Set bytes_left_to_send as MIN(bytesToTransfer, Filesize) */
    if ((unsigned long long int)file_size < bytesToTransfer) {
        bytes_left_to_send = (unsigned long long int)file_size;
    }
    else {
        bytes_left_to_send = bytesToTransfer;
    }
    if (bytes_left_to_send == 0) 
    {
        printf("bytes left to send is 0\n");
        return -1;
    }
    return 0;
}
int setup_socket(char* hostname, unsigned short int hostUDPport) {

    struct sockaddr_in receiver_address;
    struct hostent *host;
    sockfd = -1;

    /* Resolve the hostname */
    host = gethostbyname(hostname);
    if (host == NULL) {
        fprintf(stderr, "Error: Could not resolve hostname.\n");
        return -1;
        //exit(EXIT_FAILURE);
    }

    /* Create a UDP socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error opening socket");
        return -1;
        //exit(EXIT_FAILURE);
    }

    /* Set up the receiver struct */
    memset(&receiver_address, 0, sizeof(receiver_address));
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_port = htons(hostUDPport);
    memcpy(&receiver_address.sin_addr, host->h_addr_list[0], host->h_length);
    //memcpy(&receiver_address.sin_addr, host->h_addr, host->h_length);

    /* Connect other end of socket to the Receiver */ 
    if (connect(sockfd, (struct sockaddr *)&receiver_address, sizeof(receiver_address)) < 0) {
        perror("Error connecting to server");
        close(sockfd);
        return -1;
        // exit(EXIT_FAILURE);
    }

    //printf("UDP socket connected to %s:%s\n", HOSTNAME, PORT);

    /* Set the socket to non-blocking mode for receiving */
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("Error getting socket flags");
        return -1;
        //close(sockfd);
        //exit(EXIT_FAILURE);
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("Error setting socket to non-blocking mode");
        return -1;
        //close(sockfd);
        //exit(EXIT_FAILURE);
    }


    return 0;
}

void updateRTT(double sampleRTT) {
    // Update estimated RTT using new sample RTT value.
    RTT_in_ms = (1- ALPHA) * RTT_in_ms + ALPHA * sampleRTT;
    
    // Update "safety margin" for timeout intervals.
    devRTT = (1 - BETA) * devRTT + BETA * fabs(sampleRTT - RTT_in_ms);

    // Update timeout interval using this newly estimated RTT and safety margin.
    timeoutInterval_in_ms = RTT_in_ms + 4 * devRTT;
}

/* Connection Setup */
void sender_action_Start_Connection(void)
{
    /* send SYNC = 1 to receiver */ 
    struct protocol_Packet sync_packet;

    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    sync_packet.header.management_byte = 0;
    sync_packet.header.seq_ack_num = 0;
    sync_packet.header.management_byte = sync_packet.header.management_byte | 0x80;

    ssize_t bytes_sent = send(sockfd, &sync_packet, sizeof(struct protocol_Packet), 0);

    if (bytes_sent < 0) {
        perror("Error sending data");
        //TODO: handle
    }
    
    /* Start 2 second timer */
    start = clock();
    while(1)
    {
        end = clock();
        cpu_time_used_in_ms = ((double) (end - start)) / (CLOCKS_PER_SEC / 1000);

        /* Check Socket for response */
        struct protocol_Header receive_buffer;
        ssize_t bytes_received = recv(sockfd, &receive_buffer, 512, MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Sync Ack*/            
            if ((receive_buffer.management_byte & 0x40) == 0x40) {
                //TODO: set up sliding window, current packet size, RTT?
                RTT_in_ms = cpu_time_used_in_ms;
                devRTT = RTT_in_ms /2;
                timeoutInterval_in_ms = RTT_in_ms + (4 * devRTT);
                printf("timeoutInterval_in_ms %f\n", timeoutInterval_in_ms);
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
        else if (cpu_time_used_in_ms >= 2000)
        {
            break;
        }
    }
    return;    
}


/* Send Data*/
void sender_action_Send_N_Packets(void) 
{
    uint16_t sending_index;
    struct protocol_Packet packet_being_sent;
    sending_index = in_Flight[0];
    int first_packet = 1;
    
    if (in_Flight[1] > in_Flight[0])
    {
        while(sending_index <= in_Flight[1])
        {
            int i;
            packet_being_sent.header.management_byte = 0;
            packet_being_sent.header.seq_ack_num = sending_index;
            for (i = 0; (i < PROTOCOL_DATA_SIZE) && (sending_index <= in_Flight[1]); i++)
            {
                packet_being_sent.data[i] = fgetc(file_pointer);
                sending_index++; 
            }
            if (i < PROTOCOL_DATA_SIZE) {
                for (int j = i; j < PROTOCOL_DATA_SIZE; j++){
                    packet_being_sent.data[j] = EOF;
                }
            }
            ssize_t bytes_sent = send(sockfd, &packet_being_sent, sizeof(struct protocol_Packet), 0);
            printf("Sending Packet num %d\n", packet_being_sent.header.seq_ack_num);

            // TODO: error checking on send
            if (bytes_sent == -1){
                // handle
            }
            if (first_packet)
            {
                start = clock();
                first_packet = 0;
            }
        }
    }
    else if (in_Flight[0] > in_Flight[1])
    {
        while ((sending_index <= in_Flight[1]) || (sending_index >= in_Flight[0]))
        {
            int i;
            packet_being_sent.header.management_byte = 0;
            packet_being_sent.header.seq_ack_num = sending_index;
            for (i = 0; (i < PROTOCOL_DATA_SIZE && ((sending_index <= in_Flight[1]) || (sending_index >= in_Flight[0]))); i++)
            {
                packet_being_sent.data[i] = fgetc(file_pointer);
                sending_index++; 
            }
            if (i < PROTOCOL_DATA_SIZE) {
                for (int j = i; j < PROTOCOL_DATA_SIZE; j++){
                    packet_being_sent.data[j] = EOF;
                }
            }
            ssize_t bytes_sent = send(sockfd, &packet_being_sent, sizeof(struct protocol_Packet), 0);
            printf("Sending Packet num %d\n", packet_being_sent.header.seq_ack_num);
            // TODO: error checking on send
            if (bytes_sent == -1){
                // handle
            }
            if (first_packet)
            {
                start = clock();
                first_packet = 0;
            }
        }
    }
    else if (in_Flight[0] == in_Flight[1])
    {
        int i;
        packet_being_sent.header.management_byte = 0;
        packet_being_sent.header.seq_ack_num = sending_index;
        
        for (i = 0; i < PROTOCOL_DATA_SIZE; i++)
        {
            if (i == 0)
            {
                packet_being_sent.data[i] = fgetc(file_pointer);
                sending_index++; 
            }
            else 
            {
                packet_being_sent.data[i] = EOF;
            }
        }
        ssize_t bytes_sent = send(sockfd, &packet_being_sent, sizeof(struct protocol_Packet), 0);
        printf("Sending Packet num %d\n", packet_being_sent.header.seq_ack_num);
            // TODO: error checking on send
        if (bytes_sent == -1){
            // handle
        }
        if (first_packet)
        {
            start = clock();
            first_packet = 0;
        }
    }
    sender_current_state = Wait_for_Ack;
    return;
}

void sender_action_Wait_for_Ack(void)
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
        ssize_t bytes_received = recv(sockfd, &receive_buffer, sizeof(struct protocol_Header), MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Valid Seq number */
            uint16_t ack_num = receive_buffer.seq_ack_num;
            if (valid_ack_num(ack_num)) 
            {
                printf("Received Ack for up to %d\n", ack_num);
                updateRTT(cpu_time_used_in_ms);
                printf("timeoutInterval_in_ms %f\n", timeoutInterval_in_ms);
                uint16_t old_acked = acknowledged[1];
                acknowledged[1] = ack_num - 1;
                
                // update bytes left, if bytes left to send == 0, goto Send_FIN
                uint16_t difference = (acknowledged[1] - old_acked);
		        printf("difference is: %d\n", difference);
		        bytes_left_to_send = bytes_left_to_send - (difference);
                printf("%lld bytes left to send now\n", bytes_left_to_send);
                in_Flight[0] = ack_num;
                if (bytes_left_to_send == 0){
                    sender_current_state = Send_Fin;
                    break;
                }
                

                // update file offset for sending
                file_offset_for_sending =+ (acknowledged[1] - old_acked);
                fseek(file_pointer, file_offset_for_sending, SEEK_SET);
                
                //TODO: update current window size based on bytes left, AMID, theoretical max
                if (current_window_size < MAX_WINDOW_SIZE) {
                    current_window_size = current_window_size + PROTOCOL_DATA_SIZE - (current_window_size % PROTOCOL_DATA_SIZE);
                }
                else if (current_window_size >= MAX_WINDOW_SIZE)
                {
                    current_window_size = MAX_WINDOW_SIZE;
                }
                if (bytes_left_to_send < current_window_size){
                    current_window_size = bytes_left_to_send;
                }

                in_Flight[1] = in_Flight[0] + (current_window_size - 1);
                acknowledged[0] = in_Flight[1] + 1;
                printf("window size set to %d bytes\n", current_window_size);

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

        //else if timeout
            // update file offset for sending
            // update current window size AMID, theoretical max
            // go to Send N Packets
        if(cpu_time_used_in_ms > timeoutInterval_in_ms) //TODO: figure out time to use
        {
            printf("sender timed out\n");
            //TODO: update current window size based on bytes left, AMID, theoretical max
                //in_Flight[1];
                //acknowledged[0];
                // go to Send N Packets
            current_window_size = current_window_size/2;
            if ((current_window_size % PROTOCOL_DATA_SIZE) != 0)
            {
                current_window_size = current_window_size + PROTOCOL_DATA_SIZE - (current_window_size % PROTOCOL_DATA_SIZE);
            }
            if (bytes_left_to_send < current_window_size)
            {
                current_window_size = bytes_left_to_send;
            }
            in_Flight[1] = in_Flight[0] + current_window_size - 1;
            acknowledged[0] = in_Flight[1] + 1;

            printf("window size set to %d bytes\n", current_window_size);

            fseek(file_pointer, file_offset_for_sending, SEEK_SET);
            sender_current_state = Send_N_Packets;
            break;
        }

        

        // if its a valid ack
            // update bytes left, if bytes left to send == 0, goto Send_FIN
            // update file offset for sending
            // update current window size based on bytes left, AMID, theoretical max
            // go to Send N Packets
        

        
    
    }
    return;
}

int valid_ack_num(uint16_t ack_num) 
{

    if (in_Flight[0] < in_Flight[1]){
        if ((ack_num > in_Flight[0]) && (ack_num <= (in_Flight[1] + 1))){
            return 1;
        }
        return 0;
    }

    else if (in_Flight[0] > in_Flight[1])
    {   
        if ((ack_num > in_Flight[0]) || (ack_num <= (in_Flight[1] + 1)))
        {
            return 1;
        }
        return 0;
    }
    else if (in_Flight[0] == in_Flight[1]) 
    {
        if (ack_num == (in_Flight[1] + 1))
        {
            return 1;
        }
        return 0;
    }
    return 0;
}



/* Connection Teardown */
void sender_action_Send_Fin(void)
{
    /* send FIN = 1 to receiver */ 
    struct protocol_Packet fin_packet;

    /* Sync bit:7, Sync Ack bit:6, 0:5, 0:4, 0:3, 0:2, Fin bit:1, Fin ack bit:0 */
    fin_packet.header.management_byte = 0;
    fin_packet.header.seq_ack_num = 0;
    fin_packet.header.management_byte = fin_packet.header.management_byte | 0x02;

    ssize_t bytes_sent = send(sockfd, &fin_packet, sizeof(struct protocol_Packet), 0);
    printf("Sending Fin Packet\n");

    if (bytes_sent < 0) {
        perror("Error sending data");
        //TODO: handle
    }
    start = clock();

    sender_current_state = Wait_Fin_Ack;
    return;
}


void sender_action_Wait_Fin_Ack(void)
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
        ssize_t bytes_received = recv(sockfd, &receive_buffer, sizeof(struct protocol_Header), MSG_DONTWAIT);
        if (bytes_received > 0) 
        {
            /* If its a Fin Ack*/
            if ((receive_buffer.management_byte & 0x1) == 0x1) {
                //TODO: set up sliding window, current packet size, RTT?
                printf("Receiver Fin Ack\n");
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
            perror("Error receiving data\n");
            //close(sockfd);
            //exit(EXIT_FAILURE);
            // TODO: handle
        }

        /* Check Timer for timeout */ //TODO: fix timer
        else if (cpu_time_used_in_seconds >= 2)
        {   
            printf("Fin Packet timed out\n");
            sender_current_state = Send_Fin;
            break;
        }
    }
    return;    
}


void sender_finish(void){
    if (sockfd != -1) {
        close(sockfd);
    }
    if (file_pointer != NULL)
    {
        fclose(file_pointer);
    }
}

void rsend(char* hostname, 
            unsigned short int hostUDPport, 
            char* filename, 
            unsigned long long int bytesToTransfer) 
{
    if (sender_init(filename, bytesToTransfer,hostname, hostUDPport))
    {   
        sender_finish();
        return;
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
    return;
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
