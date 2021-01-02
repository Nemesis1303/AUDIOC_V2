/* parses arguments from command line for audioc */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <errno.h>
#include <string.h> 
#include "audiocArgs.h"


/*=====================================================================*/
void args_print_audioc (struct in_addr multicastIp, uint32_t ssrc, uint16_t port, uint32_t packetDuration, uint8_t payload, uint32_t bufferingTime, uint8_t vol, bool verbose)
{
    char multicastIpStr[16];
    if (inet_ntop(AF_INET, &multicastIp, multicastIpStr, 16) == NULL) {
        printf("Error converting multicast address to string\n");
        exit(-1);
    }
    printf ("Multicast IP address \'%s\'\n", multicastIpStr);
    printf ("Local SSRC (hex) %x, port %d, packet duration %d, payload %d, buffering time %d\n", ssrc, port, packetDuration, payload, bufferingTime);
    printf ("Volume %d\n", vol);
    if (verbose==1) {
        printf ("Verbose ON\n"); }
    else   {
        printf ("Verbose OFF\n");}
};

/*=====================================================================*/
static void _printHelp (void)
{
    printf ("\naudioc v2.0");
    printf ("\naudioc  MULTICAST_ADDR  LOCAL_SSRC  [-pLOCAL_RTP_PORT] [-lPACKET_DURATION] [-yPAYLOAD] [-kACCUMULATED_TIME] [-vVOL] [-c]\n\n");
}


/*=====================================================================*/
static void _defaultValues (uint16_t *port, uint8_t *vol, uint32_t *packetDuration, bool *verbose, uint8_t *payload, uint32_t *bufferingTime)
{
    *port = 5004;
    *vol = 90;
    *packetDuration = 20; /* 20 ms */

    *payload = PCMU;
    *verbose = 0; 
    *bufferingTime = 100; /* 100 ms */
};


/*=====================================================================*/
int args_capture_audioc(int argc, char * argv[], struct in_addr *multicastIp, 
uint32_t *ssrc, uint16_t *port, uint8_t *vol, uint32_t *packetDuration, 
bool *verbose, uint8_t *payload, uint32_t *bufferingTime)
{
    int index;
    char car;
    int numOfNames=0;

    /*set default values */
    _defaultValues (port, vol, packetDuration, verbose, payload, bufferingTime);

    if (argc < 3 )
    { 
        printf("\n\nNot enough arguments\n");
        _printHelp ();
        return(EXIT_FAILURE);
    }

    for ( index=1; argc>1; ++index, --argc)
    {
        if ( *argv[index] == '-')
        {   

            car = *(++argv[index]);
            switch (car)	{ 

                case 'p': /* RTP PORT*/ 
                    /* SCNu16: SCAN unsigned 16 bits, macro from <inttypes.h> */
                    if ( sscanf (++argv[index], "%" SCNu16 , port) != 1)
                    { 
                        printf ("\n-p must be followed by a number\n");
                        exit (1); /* error */
                    }		
                    if (  !  ((*port) >= 1024) )
                    {	    
                        printf ("\nPort number (-p) is out of the requested range, [1024..65535]\n");
                        exit (1); /* error */
                    }
                    break;

                case 'v': /* VOLUME */
                    if ( sscanf (++argv[index], "%" SCNu8, vol) != 1)
                    { 
                        printf ("\n-v must be followed by a number\n");
                        return(EXIT_FAILURE);
                    }
                    if (  !  ((*vol) <= 100)) 
                    {	    
                        printf ("\n-v must be followed by a number in the range [0..100]\n");
                        return(EXIT_FAILURE);
                    }
                    break;

                case 'l': /* Packet duration */
                    if ( sscanf (++argv[index], "%" SCNu32, packetDuration) != 1)
                    { 
                        printf ("\n-l must be followed by a number\n");
                        exit (1); /* error */
                    }
                    break;

                case 'c': /* VERBOSE  */
                    (*verbose) = 1;
                    break;

                case 'y': /* Initial PAYLOAD */
                    if ( sscanf (++argv[index], "%" SCNu8, payload) != 1)
                    { 
                        printf ("\n-y must be followed by a number\n");
                        exit (1); /* error */
                    }
                    if (  ! ( ((*payload) == PCMU) || ( (*payload) == L16_1)  ))
                    {	    
                        printf ("\nUnrecognized payload number. Must be either 11 or 100.\n");
                        exit (1); /* error */
                    }
                    break;

                case 'k': /* Accumulated time in buffers */
                    if ( sscanf (++argv[index],"%" SCNu32 , bufferingTime) != 1)
                    { 
                        printf ("\n-k must be followed by a number\n");
                        exit (1); /* error */
                    }
                    break;

                default:
                    printf ("\nI do not understand -%c\n", car);
                    _printHelp ();
                    return(EXIT_FAILURE);
            }

        }

        else /* There is a name */
        {
            if (numOfNames == 0) {
                if (strlen (argv[index]) > 15) 
                {
                    printf("\nInternet address (IPv4) should not have more than 15 chars\n");
                    exit (1); /* error */	
                }
                int res = inet_pton(AF_INET, argv[index], multicastIp);
                if (res < 1) {
                    printf("\nInternet address string not recognized\n");
                    return(EXIT_FAILURE);
                }
                if (!IN_CLASSD(ntohl(multicastIp ->s_addr))) {
                    printf("\nNot a multicast address\n");
                    return(EXIT_FAILURE);
                }

            }
            else if (numOfNames == 1) {
                if (sscanf (argv[index],"%u", ssrc) != 1) {
                    printf ("\nSecond argument must be a number (the local SSRC)\n");
                    return(EXIT_FAILURE);
                }
            }
            else {
                printf ("\nToo many fixed parameters - only multicastIPStr and SSRC were expected\n");
                _printHelp ();
                return(EXIT_FAILURE);
            }

            numOfNames += 1;
        }
    }

    if (numOfNames != 2)
    {
        printf("\nNeed boh multicast address and SSRC value.\n");
        _printHelp();
        return(EXIT_FAILURE);
    }
    return(EXIT_SUCCESS);
};


/* Fast test of args, uncomment the following, compile with
 * gcc -o testArgs audioArgs.c 
 * Test different entries and check the results */
/*
int main(int argc, char *argv[])
{
    struct in_addr  multicastIp;
    unsigned int ssrc;
    int port, vol, packetDuration, verbose, payload, bufferingTime;

    if (EXIT_SUCCESS == args_capture_audioc (argc, argv, &multicastIp, &ssrc, &port, &vol, &packetDuration,  &verbose,  &payload, &bufferingTime )) {
        args_print_audioc(multicastIp, ssrc, port, packetDuration, payload, bufferingTime, vol, verbose);
    }
    
    return (EXIT_SUCCESS);
}
*/
