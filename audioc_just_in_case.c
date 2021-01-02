// compilar :  gcc -Wall -Wextra -o audioc ../lib/configureSndcard.c ../lib/circularBuffer.c audiocArgs.c audioc.c
// valgrind : valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all ./audioc 227.3.4.5 1


/*INCLUDES*/
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/soundcard.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audiocArgs.h"
#include "../lib/circularBuffer.h"
#include "../lib/configureSndcard.h"
#include "../lib/rtp.h"
#include "definitions.h"

/* CONSTANT VALUES */
#define PACKET_SENT 0
#define PACKET_RECEIVED 1
#define AUDIO_BLOCK_TAKEN 2
#define SILENCE_AUDIO_BLOCK_INSERTED 3 
#define PACKET_LOST 4
#define SELECT_TIMER_EXPIRES 5

#define ZERO_8 128          // U8 range is {0,255} 
#define ZERO_16 0           // L16 range is {-32768,32767}
#define NOISE_16_DOWN 125
#define NOISE_16_MID 126
#define NOISE_16_UP_127

/* GLOBAL VARIABLES */
/*Pointers*/
void * circularBuffer = NULL;
void * package_send = NULL;
void * package_receive = NULL;
void * play_from_buffer = NULL;

/*Time variables*/
struct timeval timeSelect;
struct timeval audioc_starts;
struct timeval audioc_finishes;
struct timeval time_playing_audio_starts;
struct timeval time_playing_audio_finishes;
struct timeval diff_time_playing_audio;
struct timeval diff_time_executing_audioc;
float actual_time_playing = 0;
float theoretic_time_playing = 0;
float execution_time = 0;

/*Counters for keeping track of the number of packets that have been recorded, sent, received; silences packets inserted and played packets.*/
int number_recorded_packets = 0;
int number_sent_packets = 0;
int number_received_packets = 0;
int number_silences_inserted = 0;                 // Total number of silences inserted
int number_silences_inserted_timer_expires = 0;   // Silences inserted because timer expired since no packets were being received
int number_silences_inserted_sil_detected = 0;    // Silences inserted because silences fragments were detected when receving
int number_silences_inserted_sil_sent = 0;        // Silences inserted to replace the missing packets that were not sent due to silence detection
int number_played_packets = 0;
int number_lost_packets = 0;

/*Variables for keeping track of the sequence number and timestamp of the sent and the recevied packets*/
unsigned int sequenceNr_send = 0;       // Current sequence number used for sending 
u_int32 timestamp_send = 0;             // Current timestamo used for sending
unsigned int sequenceNr_receive = 0;    // Current sequence number used for receiving
u_int32 timestamp_receive = 0;          // Current timestamp used for receiving
unsigned int X = 0;                     // Sequence number of the previously received packet
u_int32 T = 0;                          // Timestamp of the previously received packet

/*Auxialiary void pointer in which the actual audio packet (without RTP header) is gonna be stored. We make the variable void so both U8(uint8_t) and L16 (int16_t) packets can be treated generically. */
void * audioData_package_send = NULL;
void * audioData_package_receive = NULL;

/*Auxialiary void pointer in which a silence packet is gonna be stored. Similarly as with the audioData_packets, it is used for both U8(uint8_t) and L16 (int16_t) silence packets. */
void* silence_package = NULL;

/*Contant silence arrays*/
uint8_t* SILENCE_8_ZEROS [] = {(uint8_t*) ZERO_8, (uint8_t*)ZERO_8, (uint8_t*)ZERO_8, (uint8_t*)ZERO_8, (uint8_t*)ZERO_8, (uint8_t*)ZERO_8, (uint8_t*)ZERO_8, (uint8_t*)ZERO_8}; 
int16_t* SILENCE_16_ZEROS [] = {(int16_t*) ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16, (int16_t*)ZERO_16}; 
//int16_t * SILENCE_16_NOISY_RANDOM [] =  {(int16_t*) NOISE_16_DOWN, (int16_t*)NOISE_16_MID, (int16_t*)NOISE_16_UP, (int16_t*)NOISE_16_DOWN, (int16_t*)NOISE_16_MID, (int16_t*)NOISE_UP, (int16_t*)NOISE_16_DOWN, (int16_t*)NOISE_16_MID, (int16_t*)NOISE_16_UP, (int16_t*)NOISE_16_DOWN, (int16_t*)NOISE_16_MID, (int16_t*)NOISE_16_UP, (int16_t*)NOISE_16_DOWN, (int16_t*)NOISE_16_MID, (int16_t*)NOISE_16_UP, (int16_t*)NOISE_16_UP}; 

/*Parameters that will be entered by the user*/
bool verbose = true;          // Verbose option
int bytes_per_sample = 1;     // Nr of bytes per samples (1 in PCMU, 2 in L16_1) 
int channelNumber = 1;        // Nr of channels 
int sndCardFormat = 8;        // Soundcard's format 
int rate = 8000;              // Sampling rate 
uint32_t packetDuration = 0;  // Packet duration (in milliseconds) 

/*Descriptors*/
 int socketDesc;
 int sndCardDesc;

/*
 * Function: do_free 
 * -----------------------------
 *      Frees the memory space pointed to by the created pointers returned by a call to malloc/calloc.
 */
void do_free()
{
   if (circularBuffer) cbuf_destroy_buffer(circularBuffer);
   if (package_send) free(package_send);
   if (package_receive) free(package_receive);
   if (play_from_buffer) free (play_from_buffer);
   if (silence_package) free(silence_package);
}

/*
 * Function: do_verbose 
 * -----------------------------
 *      Print the information to the screen when "-c" option is selected, according to the kind of 
 *      action specfified by type.
 *      After every print, fflush is used, so data from printf is not accumulated.
 * 
 *      type: Action type
 *          PACKET_SENT:                    Each time a packet is sent.
 *          PACKET_RECEIVED:                Each time a packet is received.
 *          AUDIO_BLOCK_TAKEN:              Each time an audio block is taken from the circular buffer and written to the soundcard.
 *          SILENCE_AUDIO_BLOCK_INSERTED:   Each time a silence audio block is inserted in the circular buffer.
 *          PACKET_LOST:                    Every time packet loss is detected.
 *          SELECT_TIMER_EXPIRES:           Every time the select timer expires.
 */
void do_verbose(int type)
{
    switch (type)
    {
        case PACKET_SENT:
            printf (".");
            break;
        case PACKET_RECEIVED:
            printf ("+");
            break;
        case AUDIO_BLOCK_TAKEN:
            printf ("-");
            break;
        case SILENCE_AUDIO_BLOCK_INSERTED:
            printf ("~");
            break;
        case PACKET_LOST:
            printf ("x");
            break;
        case SELECT_TIMER_EXPIRES:
            printf ("t");
            break;
        default:
            break;
    }
    fflush (stdout); // So data from printf is not accumulated
}

/*
 * Function: createRTPheader 
 * -----------------------------
 *      Creates a RTPHeader.
 */
void createRTPheader(rtp_hdr_t *RTPHeader, int payload, int sequenceNr, u_int32 timestamp, int ssrc)
{
    (*RTPHeader).version = 2;
    (*RTPHeader).p = 0;
    (*RTPHeader).x= 0;
    (*RTPHeader).cc= 0;
    (*RTPHeader).m= 0;
    (*RTPHeader).pt = payload;
    (*RTPHeader).seq = htons(sequenceNr); // Unsigned short integer hostshort from host byte order to network byte order.
    (*RTPHeader).ts = htonl(timestamp);   // Unsigned integer hostlong from host byte order to network byte order.
    (*RTPHeader).ssrc = htonl(ssrc);	  // Unsigned integer hostlong from host byte order to network byte order.
}

/*
 * Function: createSilencePacket 
 * -----------------------------S
 *      Creates silence audio block.
 */
void createSilencePacket(void* silence_package, int requestedFragmentSize, int nBytes, void* SILENCES)
{
    int i = 0;
    for(i = 0; i < requestedFragmentSize/(8*nBytes); ++i)
    {   
        memcpy(silence_package + 8*i*nBytes, SILENCES, 8*nBytes);
    }
}

/* activated by Ctrl-C */
void signalHandler (int sigNum __attribute__ ((unused)))  /* __attribute__ ((unused))   -> this indicates gcc not to show an 'unused parameter' warning about sigNum: is not used, but the function must be declared with this parameter */
{   
    if (gettimeofday(&audioc_finishes, NULL) < 0)
    {	
	    printf("ERROR: Taking finishing execution time failed.\n");	
	}

    printf("\n*******************************\n");
    printf("AudioC was requested to finish.\n");
    printf("*******************************\n");

    do_free();

    if(verbose)
    { 
      timersub(&audioc_finishes, &audioc_starts, &diff_time_executing_audioc);
      execution_time = (float) diff_time_executing_audioc.tv_sec + (float) diff_time_executing_audioc.tv_usec / 1000000;
      theoretic_time_playing = (number_played_packets * packetDuration) / 1000;
      timersub(&time_playing_audio_finishes, &time_playing_audio_starts, &diff_time_playing_audio);
	  actual_time_playing = (float) diff_time_playing_audio.tv_sec + (float) diff_time_playing_audio.tv_usec / 1000000;
      
      printf("STATISTICS:\n");
      printf("* Execution time (s): %f \n", execution_time);
	  printf("* Theoretical time required to playout the content (s): %f \n", theoretic_time_playing);
	  printf("* Actual time required to perform the playout (s): %f \n", actual_time_playing);
      printf("* Number of played packets: %d \n", number_played_packets);
      printf("* Number of recorded packets: %d \n", number_recorded_packets);
      printf("* Number of sent packets: %d \n", number_sent_packets);
      printf("* Number of received packets: %d \n", number_received_packets);
      printf("* Number of lost packets: %d \n", number_lost_packets);
      printf("* Total number of silences inserted: %d \n", number_silences_inserted);
      printf("** Number of silences inserted because timer expired: %d \n", number_silences_inserted_timer_expires);
      printf("** Number of silences inserted  because silences fragments were detected when receving: %d \n", number_silences_inserted_sil_detected);
      printf("** Number of silences inserted to replace the missing packets that were not sent due to silence detection: %d \n", number_silences_inserted_sil_sent);      
	}
    close(sndCardDesc);
    close(socketDesc);
    exit(0);
}

int main(int argc, char *argv[])
{
    struct sigaction sigInfo; 
    struct in_addr multicastIp;
    uint32_t ssrc;
    uint16_t port;
    uint8_t vol;

    uint32_t bufferingTime;             // In milliseconds
    uint8_t payload;

    int requestedFragmentSize;
    
    /*Circular buffer variables*/
    int numberOfBlocksBuffer;

    /*Socket variables*/
    struct sockaddr_in remToSendSAddr;  // Multicast address
    struct sockaddr_in remToRecvSAddr;
    struct ip_mreq mreq;                // For multicast configuration
    socklen_t sockAddrInLength;         // For recvfrom  


    /*Select sets*/
    fd_set readSet;
	fd_set writeSet;

    int result;                          // For storing results from system calls

    int bytesRead;                       // For storing results from read()
    int bytesWrite;                      // For storing results from write()

    int dataSent;                        // For storing results from sent()
    int dataReceived;                    // For storing results from recvfrom()

    void * pointerWriteInBuffer = NULL;  // Pointer to read and write from circular buffer
    void * pointerReadfromBuffer = NULL; // Pointer to write in circular buffer

    int nrPacketsInBuffer = 0;           // Variable to keep track of the number of packets that are in the buffer


    int nr_bytes_snd_card = 0;           // Time that is gonna take before the next sample to be written gets played by the hardware (in bytes) (use for calling to "ioctl")
    int total_nr_bytes = 0;              // Nr of bytes resulting from summing up the bytes in the sound card + the ones in the circular buffer to calculate the timer
    float time_remaining = 0.0;          // To configure the timer

    /* Obtains values from the command line - or default values otherwise */
    if (args_capture_audioc(argc, argv, &multicastIp, &ssrc,
            &port, &vol, &packetDuration, &verbose, &payload, &bufferingTime) == EXIT_FAILURE)
    { exit(1);  /* there was an error parsing the arguments, error info
                   is printed by the args_capture function */
    };
    
    /*************************/
    /* Installs signal */
    /*************************/
    sigInfo.sa_handler = signalHandler;
    sigInfo.sa_flags = 0;
    sigemptyset(&sigInfo.sa_mask); /* clear sa_mask values */
    if ((sigaction (SIGINT, &sigInfo, NULL)) < 0) 
    {
        printf("ERROR: Installing signal failed, error: %s", strerror(errno));
        exit(1);
    }

    /*************************/
    /* Configures sound card */
    /*************************/
    channelNumber = 1;;
    if (payload == PCMU)
    {
        rate = 8000;
        sndCardFormat = 8;    // Bits of the format
        bytes_per_sample = 1; // Since the bits of the format are 8, there is one byte per sample
    } else if (payload == L16_1) 
    {
        rate = 8000;
        sndCardFormat = 16;   // Bits of the format
        bytes_per_sample = 2; // since the bits of the format are 16, there are two bytes per sample
    }


    // Requested fragment size is recalculated in function of the caputured parameters
    requestedFragmentSize = (packetDuration * rate * channelNumber * bytes_per_sample) / 1000;

    /* configures sound card, sndCardDesc is filled after it returns */
    // After configuring sound card, requested fragment size is now the power of 2 immediately lower than the above calculated
    configSndcard (&sndCardDesc, &sndCardFormat, &channelNumber, &rate, &requestedFragmentSize, true);
    vol = configVol (channelNumber, sndCardDesc, vol);

    /*PRINTS from the configuration of the soundcard*/
    printf("*******************************\n");
    printf("AudioC was configured with the following parameters:\n");
    printf("- Buffering time: %i\n", bufferingTime);
    printf("- Format sndcard: %i\n", sndCardFormat);
    printf("- Channel: %i\n", channelNumber);
    printf("- Rate: %i\n", rate);
    printf("- Requested Fragment Size: %i\n", requestedFragmentSize); 

    /*Package duration once the requestedFragmentSize has been updated after calling configSndcard*/
	packetDuration = (1000 * requestedFragmentSize) / (rate * channelNumber * bytes_per_sample);
	printf("- Package duration: %i\n", packetDuration);

    /***************************/
    /* Creates circular buffer */
    /***************************/
    int guard_interval = 200; // Guard interval to guarantee to cope with possible delay variations caused by the network 
    numberOfBlocksBuffer = (int)(ceil((bufferingTime + guard_interval) / packetDuration));
    printf("- Number of blocks reserved in buffer: %i\n", numberOfBlocksBuffer);
    circularBuffer = cbuf_create_buffer(numberOfBlocksBuffer, requestedFragmentSize);
    int numberPacketsUntilPlayout = bufferingTime / packetDuration; // Variable for the while. In this case we do not take into account the guard interval that we added when creating the buffer
    printf("- Number of packets until playout: %i\n", numberPacketsUntilPlayout);
    printf("*******************************\n");

    /*************************/
    /* Configures socket */
    /*************************/
    /* Preparing bind for multicast */
    bzero(&remToSendSAddr, sizeof(remToSendSAddr));
    remToSendSAddr.sin_family = AF_INET;
    remToSendSAddr.sin_port = htons(port);
    remToSendSAddr.sin_addr = multicastIp;
    sockAddrInLength = sizeof (struct sockaddr_in); 

    /* Creates socket */
    if ((socketDesc = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("socket error\n");
        exit(EXIT_FAILURE);
    }

    /* configure SO_REUSEADDR, multiple instances can bind to the same multicast address/port */
    int enable = 1;
    if (setsockopt(socketDesc, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
        printf("setsockopt(SO_REUSEADDR) failed");
        exit(EXIT_FAILURE);
    }

    if (bind(socketDesc, (struct sockaddr *)&remToSendSAddr, sizeof(struct sockaddr_in)) < 0) {
        printf("bind error\n");
        exit(EXIT_FAILURE);
    }

    /* setsockopt configuration for joining to mcast group */
    mreq.imr_multiaddr = multicastIp;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(socketDesc, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        printf("setsockopt error");
        exit(EXIT_FAILURE);
    }

    /* Do not receive packets sent to the mcast address by this process */
    unsigned char loop=0;
    setsockopt(socketDesc, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(unsigned char));
    /*************************/
    /* End socket configuration */
    /*************************/

    /**********************************************/
    /* Defining packages for sending and receiving*/
    /**********************************************/
    package_send = malloc(sizeof(rtp_hdr_t) + requestedFragmentSize);
    if (package_send == NULL)
    {
        printf("Memory could not be reserved.");
    }

    package_receive = malloc(sizeof(rtp_hdr_t) + requestedFragmentSize);
    if (package_receive == NULL)
    {
        printf("Memory could not be reserved.");
    }

    /************************************************/
    /*DEFINING RTP HEADERS FOR SENDING AND RECEIVING*/
    /************************************************/
    // We reinterpret package_send and package_receive as rtp_hdr_t memory blocks 
    rtp_hdr_t * RTPHeader_package_send;
    RTPHeader_package_send = (rtp_hdr_t *) package_send; 

    rtp_hdr_t * RTPHeader_receive;
    RTPHeader_receive = (rtp_hdr_t *) package_receive;   

    /*********************************/
    /*CREATE SYNTHETIC SILENCE PACKET*/
    /*********************************/
    silence_package = malloc(requestedFragmentSize);
    if (payload == PCMU)
    {   
        createSilencePacket(silence_package, requestedFragmentSize, bytes_per_sample, SILENCE_8_ZEROS);
    }
    else
    {
        createSilencePacket(silence_package, requestedFragmentSize, bytes_per_sample, SILENCE_16_ZEROS);
    }

    /*************************************************/
    /*ALLOCATE MEMORY TO STORE THE AUDIO TO BE PLAYED*/
    /*************************************************/
    play_from_buffer = calloc(1,requestedFragmentSize);
    if (play_from_buffer == NULL)
    {
        printf("Memory could not be reserved.");
    }

    // Getting initial execution time
    if (gettimeofday(&audioc_starts, NULL) < 0)
    {	
	    printf("ERROR: Taking starting execution time failed.\n");	
	}

    /*********************************/
    /*BUFFER ACCUMULATION PHASE*/
    /*********************************/
    //printf("*******************************\n");
    //printf("BUFFER ACCUMULATION PHASE\n");
    //printf("Packets necessary until playout: %d\n ", numberPacketsUntilPlayout);
    //printf("*******************************\n");

    while (nrPacketsInBuffer < numberPacketsUntilPlayout)  // while the number of blocks necessary for sending to the sound card is not reached
    {   
        FD_ZERO(&readSet);              // Delete any remainder that could be in the variable 
        FD_SET(sndCardDesc, &readSet);  // Add sound card descriptor to the select reading set
        FD_SET(socketDesc, &readSet);   // Add socket descriptor to the select reading set 
        
        // Call to select with &writeSet = NULL and &timeout = NULL so it can block indefinitely
        result = select(FD_SETSIZE, &readSet, NULL, NULL, NULL); 

        if (result < 0)
        {
            printf("Error from select.\n");
        }
        else // A DESCRIPTOR IS READY FOR READING
        {  
            // SOUND CARD DESCRIPTOR READY : Audio recording + send packet 
            if (FD_ISSET(sndCardDesc, &readSet) == 1) // There is data coming into the soundcard
            {   
                audioData_package_send = (RTPHeader_package_send + 1);
                bytesRead = read(sndCardDesc, audioData_package_send, requestedFragmentSize);
                
                if (bytesRead < 0)
                {
                    printf("Error reading from soundcard, error: %s\n", strerror(errno));
                    signalHandler(SIGINT);
                }
                
                 if (bytesRead != requestedFragmentSize)
                {
                    printf ("Recorded a different number of bytes than expected (recorded %d bytes, expected %d)\n", bytesRead, requestedFragmentSize);
                    signalHandler(SIGINT);
                }
                else
                {   
                    // A package has been recorded in the sound card
                    ++number_recorded_packets;

                    // Fill RTPHeader from the packet that is gonna be send
                    createRTPheader(RTPHeader_package_send, payload, sequenceNr_send, timestamp_send, ssrc);

                    // Data is sent independently of if silences are contained or not
                    dataSent = sendto(socketDesc, package_send, (sizeof(rtp_hdr_t) + requestedFragmentSize) , 0 , (struct sockaddr *) &remToSendSAddr, sizeof(remToSendSAddr));

                    if (dataSent < 0)
                    {
                        printf("ERROR: Sendto error.\n");
                        signalHandler(SIGINT);
                    }
                    else
                    {   
                        ++number_sent_packets;
                        // Update timestamp and seq nr for the next packet
                        ++sequenceNr_send;
                        timestamp_send +=  requestedFragmentSize / bytes_per_sample;
                        if(verbose) do_verbose(PACKET_SENT);
                    }
                }
            }

            // SOCKET DESCRIPTOR READY : Receive from socket + store in circular buffer
            if (FD_ISSET(socketDesc, &readSet) == 1) 
            {
                dataReceived = recvfrom(socketDesc, package_receive, (sizeof(rtp_hdr_t) + requestedFragmentSize) , 0 , (struct sockaddr *) &remToRecvSAddr, &sockAddrInLength);

                if (dataReceived < 0)  
                {
                    printf("ERROR: recvfrom error\n");
                    signalHandler(SIGINT);
                }
                else 
                {   
                    // A packet has been received
                    ++number_received_packets;
                    if(verbose) do_verbose(PACKET_RECEIVED);

                    // Update sequence number and timestamp from received packets
                    sequenceNr_receive = ntohs(RTPHeader_receive -> seq);
                    timestamp_receive = ntohl(RTPHeader_receive -> ts);

                    // Put data received from socket into circular buffer
                    audioData_package_receive = (RTPHeader_receive+1);
                    if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
                    {   
                        memcpy(pointerWriteInBuffer, audioData_package_receive, requestedFragmentSize);
                        ++nrPacketsInBuffer;
                    }
                    else
                    {
                        printf("Buffer full.");
                    } 

                    // Keep track of sequence number and timestamp for next iteration
                    X = sequenceNr_receive;
                    T = timestamp_receive; 
                }
            }
        }
    } //end while buffer accumulation


    /*********************************/
    /*STEADY REGIME*/
    /*********************************/
    //printf("*******************************\n");
    //printf("STEADY REGIME\n");
    //printf("*******************************\n");
   while(true)
   {   
        FD_ZERO(&readSet);              // Delete any remainder that could be in the variable 
        FD_ZERO(&writeSet);             // Delete any remainder that could be in the variable
        FD_SET(socketDesc, &readSet);   // Add socket descriptor to the select reading set 
        FD_SET(sndCardDesc, &readSet);  // Add sound card descriptor to the select reading set
        FD_SET(sndCardDesc, &writeSet); // Add sound card descriptor to the select wrting set

        // The timer needs to be configured so both the packets that are left in the sound card, as well as the ones that are left in the buffer
        // are taken into account. Otherwise, we could get into underrun
        ioctl(sndCardDesc, SNDCTL_DSP_GETODELAY, &nr_bytes_snd_card); 
        total_nr_bytes = nr_bytes_snd_card + (nrPacketsInBuffer * (requestedFragmentSize/bytes_per_sample)); // Total nr bytes = left in sndCard + the ones from buffer
        time_remaining = ((float) total_nr_bytes) / (rate * channelNumber * bytes_per_sample);
       
        // The timer is configured to expire 10 ms before the data is exhausted, so that the code has enough time to insert a silence packet before the user may notice.
        time_remaining -= 0.01; 
        if (time_remaining < 0) time_remaining = 0;

        timeSelect.tv_sec = (long)(int) time_remaining;
        timeSelect.tv_usec = ((time_remaining - timeSelect.tv_sec) * 1000000);

        // Only if there are audio blocks in the circular buffer, the select system requests a write operation in the soundcard
        if (nrPacketsInBuffer > 0)
        {
            result = select (FD_SETSIZE, &readSet, &writeSet, NULL, &timeSelect); // There is data in the buffer; write op is requested
        }
        else
        {
            result = select (FD_SETSIZE, &readSet, NULL, NULL, &timeSelect);     // There is no data in the buffer; write op is not requested
        }

        if (result < 0)
        {
            printf("Error from select.\n");
        } 
        else if (result == 0) // If data packets are not received, the timer will eventually expire
        {   
            if (verbose) do_verbose(SELECT_TIMER_EXPIRES);         
            // A silence audio fragment is inserted and the timestamp value is updated as if a packet would have actually been received
            if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
            {
                memcpy(pointerWriteInBuffer, silence_package, requestedFragmentSize);
                ++nrPacketsInBuffer;
                ++number_silences_inserted;
                ++number_silences_inserted_timer_expires;
                timestamp_receive += requestedFragmentSize / bytes_per_sample;
                T = timestamp_receive;
            }
        } 
        else 
        {   
            // SOUND CARD DESCRIPTOR READING READY -> Once data is available (recorded) at the soundcard, it is immediately sent to the remote system.
            // No buffering performed when sending to the other side.
            if (FD_ISSET(sndCardDesc, &readSet) == 1) 
            {   
                audioData_package_send = (RTPHeader_package_send + 1);
                bytesRead = read(sndCardDesc, audioData_package_send, requestedFragmentSize);
               
                if (bytesRead < 0) 
                {
                    printf("Error reading from soundcard, error: %s\n", strerror(errno));
                    signalHandler(SIGINT);
                }
                if (bytesRead != requestedFragmentSize)
                {
                    printf ("Recorded a different number of bytes than expected (recorded %d bytes, expected %d)\n", bytesRead, requestedFragmentSize);
                    signalHandler(SIGINT);
                }
                else 
                {   
                    // A package has been recorded
                    ++number_recorded_packets;

                    // Create RTP Header 
                    createRTPheader(RTPHeader_package_send, payload, sequenceNr_send, timestamp_send, ssrc);

                    dataSent = sendto(socketDesc, package_send, (sizeof(rtp_hdr_t) + requestedFragmentSize) , 0 , (struct sockaddr *) &remToSendSAddr, sizeof(remToSendSAddr));

                    if (dataSent < 0)
                    {
                        printf("ERROR: Sendto error.\n");
                        signalHandler(SIGINT);
                    }
                    else
                    {
                        ++number_sent_packets;
                        // Update timestamp and seq nr for the next packet
                        ++sequenceNr_send;
                        timestamp_send += requestedFragmentSize/bytes_per_sample;
                        if(verbose) do_verbose(PACKET_SENT);
                    }
                }
            } // end soundcard descriptor reading ready

            // SOCKET DESCRIPTOR READING READY  ->  Once data is received from the remote node (recvfrom system call), the audio data is copied
            // into the circular buffer.
            if (FD_ISSET(socketDesc, &readSet) == 1) 
            {
                dataReceived = recvfrom(socketDesc, package_receive, (sizeof(rtp_hdr_t) + requestedFragmentSize) , 0 , (struct sockaddr *) &remToRecvSAddr, &sockAddrInLength);

                if (dataReceived < 0)  
                {
                    printf("ERROR: recvfrom error\n");
                    signalHandler(SIGINT);
                }
                else 
                {   
                    // A packet has been received
                    ++number_received_packets;
                    if(verbose) do_verbose(PACKET_RECEIVED);

                    // Update sequence number from received packets
                    sequenceNr_receive = ntohs(RTPHeader_receive -> seq);
                    timestamp_receive = ntohl(RTPHeader_receive -> ts);

                    // Detect silences and late packets
                    int F = requestedFragmentSize/bytes_per_sample;
                    
                    // CASE 1: EVERYTHING OK: Data is copied normally into the circular buffer
                    if((sequenceNr_receive == (X+1)) && (timestamp_receive == (T+F))) 
                    {
                        audioData_package_receive = (RTPHeader_receive+1);
                        if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
                        {   
                            memcpy(pointerWriteInBuffer, audioData_package_receive, requestedFragmentSize);
                            ++nrPacketsInBuffer;
                        }  
                        else
                        {
                            printf("Buffer full.");
                            signalHandler(SIGINT);
                        }
                    }
                    // CASE 2: SILENCES DETECTED: One or more silences have been detected (and not sent) but no packet loss has occured
                    else if((sequenceNr_receive == (X+1)) && (timestamp_receive > (T+F)))
                    {
                        // Insert in the circular buffer the appropiate number of silence fragments
                        int nr_silences_packets_to_insert = (timestamp_receive - (T+F)) / F;
                        int s = 0;
                        for(s = 0; s < nr_silences_packets_to_insert; ++s)
                        {
                            if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
                            {   
                                memcpy(pointerWriteInBuffer, silence_package, requestedFragmentSize); 
                                ++nrPacketsInBuffer;
                                ++number_silences_inserted;
                                ++number_silences_inserted_sil_detected;
                                if (verbose) do_verbose(SILENCE_AUDIO_BLOCK_INSERTED);
                            }  
                            else
                            {
                                printf("Buffer full.");
                                signalHandler(SIGINT);
                            }       
                        }

                        // Insert the received packet
                        audioData_package_receive = (RTPHeader_receive+1);
                        if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
                        {   
                            memcpy(pointerWriteInBuffer, audioData_package_receive, requestedFragmentSize);
                            ++nrPacketsInBuffer;
                        }  
                        else
                        {
                            printf("Buffer full.");
                            signalHandler(SIGINT);
                        }
                    }
                    // CASE 3: LOST PACKETS: If we receive a packet with X, and then X+2, a packet has been lost 
                    else if(sequenceNr_receive > (X+1))
                    {
                        // When the sequence number of the packet received is X+K, then K-1 packets have been lost
                        int nr_packets_lost = sequenceNr_receive - (X+1);
                        int K = nr_packets_lost + 1; 
                        number_lost_packets += nr_packets_lost;
                        if(verbose)
                        {
                            int pl = 0;
                            for(pl = 0; pl < nr_packets_lost; ++pl)
                            {
                                do_verbose(PACKET_LOST);
                            }
                        }

                        // CASE 3.1: TIMESTAMP LOWER THAN EXPECTED: The packet is always discarded    
                        if (timestamp_receive < (T + K*F)) 
                        { 
					        printf("Packet discarded because timestamp was lower than expected.\n");
				        }
                        // CASE 3.2: TIMESTAMP OK
                        // Timestamp has the form of T+(K+J)*F, with J being the number of packets that were not sent due to silence detection. 
                        // The reaction to this will be to insert the K+J-1 silent packets (as commented in the previous case) to replace missing packets
                        else
                        {   
                            // timestamp = T + (K*J)*F -> J = (timestamp - T)/F - K 
                            int J = (timestamp_receive - T)/F - K; // Nr of packets that were not sent due to silence detection
                            int nr_silences_packets_to_insert = K + J - 1;

                            // Insert in the circular buffer the appropiate number of silence fragments
                            int s = 0;
                            for(s = 0; s < nr_silences_packets_to_insert; ++s)
                            {
                                if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
                                {   
                                    memcpy(pointerWriteInBuffer, silence_package, requestedFragmentSize);
                                    ++nrPacketsInBuffer;
                                    ++number_silences_inserted;
                                    ++number_silences_inserted_sil_sent;
                                    if (verbose) do_verbose(SILENCE_AUDIO_BLOCK_INSERTED);
                                }  
                                else
                                {
                                    printf("Buffer full.");
                                    signalHandler(SIGINT);
                                }       
                            }

                            // Insert the received packet
                            audioData_package_receive = (RTPHeader_receive+1);
                            if ((pointerWriteInBuffer = cbuf_pointer_to_write(circularBuffer)) != NULL)
                            {   
                                memcpy(pointerWriteInBuffer, audioData_package_receive, requestedFragmentSize);
                                ++nrPacketsInBuffer;
                            }  
                            else
                            {
                                printf("Buffer full.");
                                signalHandler(SIGINT);
                            }

                        } 
                    }
                    // Keep track of sequence number and timestamp for next iteration
                    X = sequenceNr_receive;
                    T = timestamp_receive; 
                 }
            } // end socket descriptor reading ready

            // SOUND CARD DESCRIPTOR WRITING READY 
            if (FD_ISSET(sndCardDesc, &writeSet) == 1) 
            {
                if (cbuf_has_block(circularBuffer) == 1)
                {   
                    // We take the packet from the buffer
                    pointerReadfromBuffer = cbuf_pointer_to_read(circularBuffer);
                    
                    if(pointerReadfromBuffer) // If the packet is not null
                    {   
                        // We write into the sound card, that is, data is reporduced
                        bytesWrite = write(sndCardDesc, play_from_buffer, requestedFragmentSize);
                       
                        if (bytesWrite < 0)
                        {
                            printf("Error writing in the soundcard, error: %s\n", strerror(errno));
                            signalHandler(SIGINT);
                        } 
                        else 
                        {
                            if (bytesWrite != requestedFragmentSize)
                            {
                                printf ("Played a different number of bytes than expected (played %d bytes, expected %d; exiting)\n", bytesWrite, requestedFragmentSize);
                                signalHandler(SIGINT);
                            }

                            if (verbose) do_verbose(AUDIO_BLOCK_TAKEN);
                         
                            if (number_played_packets == 0)
                            {
                                if (gettimeofday(&time_playing_audio_starts, NULL) < 0)
                                {	
		                            printf("ERROR: Taking intial time of playing audio failed.\n");	
	                            }
                            }
                            else
                            {
                                if (gettimeofday(&time_playing_audio_finishes, NULL) < 0)
                                {	
		                            printf("ERROR: Taking final time of playing audio failed.\n");	
	                            }
                            }
                            --nrPacketsInBuffer;
                            ++number_played_packets;
                        }
                    }
                    else 
                    {
                        printf("There is no data in the buffer to be written in the sound card. \n");
                    }
                }
            } // end soundCard descriptor writing ready
        } // end res ! = 0
   } // end while(True)

   do_free();
   return 0;
}// end main