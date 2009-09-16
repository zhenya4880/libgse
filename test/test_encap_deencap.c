/****************************************************************************/
/**
 * @file    test_encap_deencap.c
 * @brief   GSE encapsulation and deencapsulation test
 * @author  Didier Barvaux / Viveris Technologies
 * @author  Julien Bernard / Viveris Technologies
 */
/****************************************************************************/

/****************************************************************************
 *
 *   INCLUDES
 *
 *****************************************************************************/

/* system includes */
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <net/ethernet.h>
#include <stdint.h>

/* include for the PCAP library */
#include <pcap.h>

/* GSE includes */
#include "gse_encap_fct.h"
#include "gse_deencap_fct.h"
#include "gse_encap.h"
#include "gse_deencap.h"

/****************************************************************************
 *
 *   MACROS AND CONSTANTS
 *
 *****************************************************************************/

/** A very simple maximum macro */
#define MAX(x, y)  (((x) > (y)) ? (x) : (y))

/** A very simple minimum macro */
#define MIN(x, y)  (((x) < (y)) ? (x) : (y))

/** The program usage */
#define TEST_USAGE \
"GSE test application: test the GSE library with a flow of IP packets\n\n\
usage: test [-verbose] cmp_file flow\n\
  verbose         Print DEBUG information\n\
  frag_length     maximum length of the GSE packets\n\
  flow            flow of Ethernet frames (PCAP format)\n"


/** The length of the Linux Cooked Sockets header */
#define LINUX_COOKED_HDR_LEN  16

#define QOS_NBR 1
#define FIFO_SIZE 100
#define PKT_MAX 1000
#define PROTOCOL 9029

/** DEBUG macro */
#define DEBUG(verbose, format, ...) \
  do { \
    if(verbose) \
      printf(format, ##__VA_ARGS__); \
  } while(0)

/****************************************************************************
 *
 *   PROTOTYPES OF PRIVATE FUNCTIONS
 *
 *****************************************************************************/

static int test_encap(int verbose, size_t frag_length, char *filename);
static int compare_packets(int verbose,
                           unsigned char *pkt1, int pkt1_size,
                           unsigned char *pkt2, int pkt2_size);


/****************************************************************************
 *
 *   PUBLIC FUNCTIONS
 *
 *****************************************************************************/


/**
 * @brief Main function for the GSE test program
 *
 * @param argc  the number of program arguments
 * @param argv  the program arguments
 * @return      the unix return code:
 *               \li 0 in case of success,
 *               \li 1 in case of failure
 */
int main(int argc, char *argv[])
{
  char *filename = NULL;
  char *frag_length = 0;
  int failure = 1;

  /* parse program arguments, print the help message in case of failure */
  if((argc < 3) || (argc > 4))
  {
    printf(TEST_USAGE);
    goto quit;
  }

  if(argc == 3)
  {
    frag_length = argv[1];
    filename = argv[2];
    failure = test_encap(0, atoi(frag_length), filename);
  }
  if(argc == 4)
  {
    if(strcmp(argv[1], "verbose"))
    {
      printf(TEST_USAGE);
      goto quit;
    }
    frag_length = argv[2];
    filename = argv[3];
    failure = test_encap(1, atoi(frag_length), filename);
  }

quit:
  return failure;
}

/****************************************************************************
 *
 *   PRIVATE FUNCTIONS
 *
 *****************************************************************************/


/**
 * @brief Test the GSE library with a flow of IP or GSE packets
 *
 * @param verbose       0 for no debug messages, 1 for debug
 * @param frag_length   Maximum length of fragments
 * @param filename      The name of the PCAP file
 * @return              0 in case of success, 1 otherwise
 */
static int test_encap(int verbose, size_t frag_length, char *filename)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *handle;
  pcap_t *cmp_handle;
  int link_layer_type_src;
  int link_layer_type_cmp;
  uint32_t link_len_src;
  uint32_t link_len_cmp;
  struct pcap_pkthdr header;
  struct pcap_pkthdr cmp_header;
  unsigned char *packet;
  unsigned char *cmp_packet;
  int is_failure = 1;
  unsigned long counter;
  gse_encap_t *encap = NULL;
  gse_deencap_t *deencap = NULL;
  vfrag_t **vfrag_pkt = NULL;
  int pkt_nbr = 0;
  int rcv_pkt_nbr = 0;
  uint8_t label[6];
  vfrag_t *pdu = NULL;
  vfrag_t *rcv_pdu = NULL;
  uint8_t rcv_label[6];
  uint8_t label_type;
  uint16_t protocol;
  uint16_t gse_length;
  int i;
  int status;
  uint8_t qos = 0;

  /* open the source dump file */
  handle = pcap_open_offline(filename, errbuf);
  if(handle == NULL)
  {
    DEBUG(verbose, "failed to open the source pcap file: %s\n", errbuf);
    goto error;
  }

  /* link layer in the source dump must be supported */
  link_layer_type_src = pcap_datalink(handle);
  if(link_layer_type_src != DLT_EN10MB &&
     link_layer_type_src != DLT_LINUX_SLL &&
     link_layer_type_src != DLT_RAW)
  {
    DEBUG(verbose, "link layer type %d not supported in source dump (supported = "
           "%d, %d, %d)\n", link_layer_type_src, DLT_EN10MB, DLT_LINUX_SLL,
           DLT_RAW);
    goto close_input;
  }

  if(link_layer_type_src == DLT_EN10MB)
    link_len_src = ETHER_HDR_LEN;
  else if(link_layer_type_src == DLT_LINUX_SLL)
    link_len_src = LINUX_COOKED_HDR_LEN;
  else /* DLT_RAW */
    link_len_src = 0;

  /* open the comparison dump file */
  cmp_handle = pcap_open_offline(filename, errbuf);
  if(cmp_handle == NULL)
  {
    DEBUG(verbose, "failed to open the comparison pcap file: %s\n", errbuf);
    goto close_input;
  }

  /* link layer in the comparison dump must be supported */
  link_layer_type_cmp = pcap_datalink(cmp_handle);
  if(link_layer_type_cmp != DLT_EN10MB &&
     link_layer_type_cmp != DLT_LINUX_SLL &&
     link_layer_type_cmp != DLT_RAW)
  {
    DEBUG(verbose, "link layer type %d not supported in comparison dump "
           "(supported = %d, %d, %d)\n", link_layer_type_cmp, DLT_EN10MB,
           DLT_LINUX_SLL, DLT_RAW);
    goto close_comparison;
  }

  if(link_layer_type_cmp == DLT_EN10MB)
    link_len_cmp = ETHER_HDR_LEN;
  else if(link_layer_type_cmp == DLT_LINUX_SLL)
    link_len_cmp = LINUX_COOKED_HDR_LEN;
  else /* DLT_RAW */
    link_len_cmp = 0;

  /* Initialize the GSE library */
  status = gse_encap_init(QOS_NBR, FIFO_SIZE, &encap);
  if(status != STATUS_OK)
  {
    DEBUG(verbose, "Error %#.4x when initializing encapsulation (%s)\n", status, gse_get_status(status));
    goto close_comparison;
  }
  status = gse_deencap_init(QOS_NBR, &deencap);
  if(status != STATUS_OK)
  {
    DEBUG(verbose, "Error %#.4x when initializing deencapsulation (%s)\n", status, gse_get_status(status));
    goto release_encap;
  }

  vfrag_pkt = malloc(sizeof(vfrag_t*) * PKT_MAX);

  /* for each packet in the dump */
  counter = 0;
  while((packet = (unsigned char *) pcap_next(handle, &header)) != NULL)
  {
    unsigned char *in_packet;
    size_t in_size;

    counter++;

    /* check Ethernet frame length */
    if(header.len <= link_len_src || header.len != header.caplen)
    {
      DEBUG(verbose, "packet #%lu: bad PCAP packet (len = %d, caplen = %d)\n",
             counter, header.len, header.caplen);
      goto release_lib;
    }

    in_packet = packet + link_len_src;
    in_size = header.len - link_len_src;

    /* Encapsulate the input packets, use in_packet and in_size as
       input */
    for(i=0 ; i<6 ; i++)
      label[i] = i;
    status = gse_create_vfrag_with_data(&pdu, in_size, in_packet, in_size);
    if(status != STATUS_OK)
    {
      DEBUG(verbose, "Error %#.4x when creating virtual fragment (%s)\n", status, gse_get_status(status));
      goto release_lib;
    }

    status = gse_encap_receive_pdu(pdu, encap, label, 0, ntohs(PROTOCOL), qos);
    if(status != STATUS_OK)
    {
      DEBUG(verbose, "Error %#.4x when receiving pdu (%s)\n", status, gse_get_status(status));
      goto release_lib;
    }
    qos++;
  }

  for(i = 0 ; i < QOS_NBR ; i++)
  {
    pkt_nbr = 0;
    rcv_pkt_nbr = 0;

    do{
      status = gse_encap_get_packet_copy(&vfrag_pkt[pkt_nbr], encap, frag_length, i);
      if(status == STATUS_OK)
      {
        pkt_nbr++;
      }
      if((status != STATUS_OK) && (status != FIFO_EMPTY))
      {
        DEBUG(verbose, "Error %#.4x when getting packet (%s)\n", status, gse_get_status(status));
        goto free_packets;
      }
    }while(status != FIFO_EMPTY);

    DEBUG(verbose, "PDU #%lu: %d packets copied in FIFO %d\n", counter, pkt_nbr, i);

    do{
      status = gse_deencap_packet(vfrag_pkt[rcv_pkt_nbr], deencap, &label_type, rcv_label,
                                  &protocol, &rcv_pdu, &gse_length);
      rcv_pkt_nbr++;
      if((status != STATUS_OK) && (status != PDU))
      {
        DEBUG(verbose, "Error %#.4x when deencapsulating packet (%s)\n", status, gse_get_status(status));
        goto free_packets;
      }
      DEBUG(verbose, "GSE packet #%d received, GSE Length = %d\n", rcv_pkt_nbr - 1, gse_length);
      vfrag_pkt[rcv_pkt_nbr - 1] = NULL;
    }while((status != PDU) || (rcv_pkt_nbr < pkt_nbr));

    cmp_packet = (unsigned char *) pcap_next(cmp_handle, &cmp_header);
    if(cmp_packet == NULL)
    {
      DEBUG(verbose, "packet #%lu: no packet available for comparison\n", counter);
      goto free_pdu;
    }

    /* compare the output packets with the ones given by the user */
    if(cmp_header.caplen <= link_len_cmp)
    {
      DEBUG(verbose, "packet #%lu: packet available for comparison but too small\n",
            counter);
      goto free_pdu;
    }

    if(!compare_packets(verbose, rcv_pdu->start, rcv_pdu->length,
                        cmp_packet + link_len_cmp, cmp_header.caplen - link_len_cmp))
    {
      DEBUG(verbose, "packet #%lu: generated packet is not as attended\n", counter);
      goto free_pdu;
    }
    DEBUG(verbose, "Complete PDU #%lu:\nLabel Type: %d | Protocol: %#.4x | Label: %.2d",
          counter, label_type, protocol, rcv_label[0]);
    for(i = 1 ; i < gse_get_label_length(label_type) ; i++)
    {
      DEBUG(verbose, ":%.2d", rcv_label[i]);
    }
    DEBUG(verbose, " (in hexa)\n");
    if((label_type != 0) && (protocol != PROTOCOL))
    {
      DEBUG(verbose, "---------- BAD PARAMETERS VALUE ----------\n");
      goto free_pdu;
    }
    for(i = 0 ; i < gse_get_label_length(label_type) ; i++)
    {
      if(rcv_label[i] != label[i])
      {
        DEBUG(verbose, "---------- BAD PARAMETERS VALUE ----------\n");
        goto free_pdu;
      }
    }

    if(rcv_pdu != NULL)
    {
      status = gse_free_vfrag(rcv_pdu);
      if(status != STATUS_OK)
      {
        DEBUG(verbose, "Error %#.4x when destroying pdu (%s)\n", status, gse_get_status(status));
        goto free_pdu;
      }
      rcv_pdu = NULL;
    } 
  }

  /* everything went fine */
  is_failure = 0;

free_pdu:
  if(rcv_pdu != NULL)
  {
    status = gse_free_vfrag(rcv_pdu);
    if(status != STATUS_OK)
    {
      is_failure = 1;
      DEBUG(verbose, "Error %#.4x when destroying pdu (%s)\n", status, gse_get_status(status));
    }
  }
free_packets:
  for(i = rcv_pkt_nbr ; i < pkt_nbr ; i++)
  {
    if(vfrag_pkt[i] != NULL)
    {
      status = gse_free_vfrag(vfrag_pkt[i]);
      if((status != STATUS_OK) && (status != FIFO_EMPTY))
      {
        DEBUG(verbose, "Error %#.4x when destroying packet (%s)\n", status, gse_get_status(status));
        is_failure = 1;
      }
    }
  }
release_lib:
  if(vfrag_pkt != NULL)
  {
    free(vfrag_pkt);
  }
  status = gse_deencap_release(deencap);
  if(status != STATUS_OK)
  {
    is_failure = 1;
    DEBUG(verbose, "Error %#.4x when releasing deencapsulation (%s)\n", status, gse_get_status(status));
  }
release_encap:
  status = gse_encap_release(encap);
  if(status != STATUS_OK)
  {
    is_failure = 1;
    DEBUG(verbose, "Error %#.4x when releasing encapsulation (%s)\n", status, gse_get_status(status));
  }
close_comparison:
  pcap_close(cmp_handle);
close_input:
  pcap_close(handle);
error:
  return is_failure;
}


/**
 * @brief Compare two network packets and print differences if any
 *
 * @param pkt1      The first packet
 * @param pkt1_size The size of the first packet
 * @param pkt2      The second packet
 * @param pkt2_size The size of the second packet
 * @return          Whether the packets are equal or not
 */
static int compare_packets(int verbose,
                           unsigned char *pkt1, int pkt1_size,
                           unsigned char *pkt2, int pkt2_size)
{
  int valid = 1;
  int min_size;
  int i, j, k;
  char str1[4][7], str2[4][7];
  char sep1, sep2;

  min_size = pkt1_size > pkt2_size ? pkt2_size : pkt1_size;

  /* do not compare more than 180 bytes to avoid huge output */
  min_size = MIN(180, min_size);

  /* if packets are equal, do not print the packets */
  if(pkt1_size == pkt2_size && memcmp(pkt1, pkt2, pkt1_size) == 0)
    goto skip;

  /* packets are different */
  valid = 0;

  DEBUG(verbose, "------------------------------ Compare ------------------------------\n");

  if(pkt1_size != pkt2_size)
  {
    DEBUG(verbose, "packets have different sizes (%d != %d), compare only the %d "
           "first bytes\n", pkt1_size, pkt2_size, min_size);
  }

  j = 0;
  for(i = 0; i < min_size; i++)
  {
    if(pkt1[i] != pkt2[i])
    {
      sep1 = '#';
      sep2 = '#';
    }
    else
    {
      sep1 = '[';
      sep2 = ']';
    }

    sprintf(str1[j], "%c0x%.2x%c", sep1, pkt1[i], sep2);
    sprintf(str2[j], "%c0x%.2x%c", sep1, pkt2[i], sep2);

    /* make the output human readable */
    if(j >= 3 || (i + 1) >= min_size)
    {
      for(k = 0; k < 4; k++)
      {
        if(k < (j + 1))
          DEBUG(verbose, "%s  ", str1[k]);
        else /* fill the line with blanks if nothing to print */
          DEBUG(verbose, "        ");
      }

      DEBUG(verbose, "      ");

      for(k = 0; k < (j + 1); k++)
        DEBUG(verbose, "%s  ", str2[k]);

      DEBUG(verbose, "\n");

      j = 0;
    }
    else
    {
      j++;
    }
  }

  DEBUG(verbose, "----------------------- packets are different -----------------------\n");

skip:
  return valid;
}
