#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <math.h>

#include "sbn_client.h"

#define  CFE_SBN_CLIENT_NO_PROTOCOL   0



// TODO: can this be included instead of duplicated here?
// #define CCSDS_TIME_SIZE 6 // <- see mps_mission_cfg.h

// Refer to sbn_cont_tbl.c to make sure these match
// SBN is running here: <- Should be in the platform config
#define SBN_CLIENT_PORT    1234
#define SBN_CLIENT_IP_ADDR "127.0.0.1"

// Private functions
int32 SBN_ClientInit(void);
int connect_to_server(const char *server_ip, uint16_t server_port);
int send_msg(int sockfd, CFE_SB_Msg_t *msg);
int send_heartbeat(int sockfd);
int recv_msg(int sockfd);
pthread_mutex_t receive_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  received_condition = PTHREAD_COND_INITIALIZER;
///////////////////////


// TODO: Our use of sockfd is not uniform. Should pass to each function XOR use as global
int sockfd = 0;
int cpuId = 0;
struct sockaddr_in server_address;

void *heartbeatMinder(void *vargp);
pthread_t heart_thread_id;
void *receiveMinder(void *vargp);
pthread_t receive_thread_id;
CFE_SBN_Client_PipeD_t PipeTbl[CFE_PLATFORM_SBN_CLIENT_MAX_PIPES];
MsgId_to_pipes_t MsgId_Subscriptions[CFE_SBN_CLIENT_MSG_ID_TO_PIPE_ID_MAP_SIZE];
static void SendSubToSbn(int SubType, CFE_SB_MsgId_t MsgID, CFE_SB_Qos_t QoS);
void invalidate_pipe(CFE_SBN_Client_PipeD_t *pipe);

int32 check_pthread_create_status(int status, int32 errorId)
{
    int32 thread_status;
    
    if (status == 0)
    {
        thread_status = SBN_CLIENT_SUCCESS; 
    }
    else
    {
        switch(status)
        {
            case EAGAIN:
            puts("Create thread error = EAGAIN");
            break;
            
            case EINVAL:
            puts("Create thread error = EINVAL");
            break;
            
            case EPERM:
            puts("Create thread error = EPERM");
            break;
            
            default:
            printf("Unknown thread creation error = %d\n", status);        
        }
        
        perror("pthread_create error");
        
        thread_status = errorId;
    }/* end if */
    
    return thread_status;
}

int32 SBN_ClientInit(void)
{
     /* Gets socket file descriptor */
    int32 Status = SBN_CLIENT_NO_STATUS_SET;
    int heart_thread_status = 0;
    int receive_thread_status = 0;
    
    printf("SBN_Client Connecting to %s, %d\n", SBN_CLIENT_IP_ADDR, 
        SBN_CLIENT_PORT);
    
    sockfd = connect_to_server(SBN_CLIENT_IP_ADDR, SBN_CLIENT_PORT);
    cpuId = 2; /* TODO: this is hardcoded, but should be set by cFS's SBN ??*/

    if (sockfd < 0)
    {
        puts("SBN_CLIENT: ERROR Failed to get sockfd, cannot continue.");
        Status = SBN_CLIENT_BAD_SOCK_FD_EID;
    }
    else
    {
        /* Create pipe table */
        CFE_SBN_Client_InitPipeTbl();

        /* Create thread for watchdog and receive */
        heart_thread_status = pthread_create(&heart_thread_id, NULL, 
            heartbeatMinder, NULL);
            
        Status = check_pthread_create_status(heart_thread_status, 
            SBN_CLIENT_HEART_THREAD_CREATE_EID);
        
        if (Status == SBN_CLIENT_SUCCESS)
        {    
            receive_thread_status = pthread_create(&receive_thread_id, NULL, 
            receiveMinder, NULL);
        
            Status = check_pthread_create_status(receive_thread_status, 
                SBN_CLIENT_RECEIVE_THREAD_CREATE_EID);
        }/* end if */ 
        
    }/* end if */ 
    
    if (Status != SBN_CLIENT_SUCCESS)
    {
        printf("SBN_ClientInit error %d\n", Status);
        exit(Status);
    }/* end if */ 
    
    return Status;
}/* end SBN_ClientInit */



/* message_entry_point determines which slot a new message enters the pipe.
 * the mod allows it to go around the bend easily, i.e. 2 + 4 % 5 = 1, 
 * slots 2,3,4,0 are taken so 1 is entry */
int message_entry_point(CFE_SBN_Client_PipeD_t pipe)
{
    return (pipe.ReadMessage + pipe.NumberOfMessages) % CFE_PLATFORM_SBN_CLIENT_MAX_PIPE_DEPTH;
}

int CFE_SBN_CLIENT_ReadBytes(int sockfd, unsigned char *msg_buffer, size_t MsgSz)
{
    int bytes_received = 0;
    int total_bytes_recd = 0;
    
    //TODO:Some kind of timeout on this?
    while (total_bytes_recd != MsgSz)
    {
        bytes_received = read(sockfd, msg_buffer + total_bytes_recd, MsgSz - total_bytes_recd);
        
        if (bytes_received < 0)
        {
            //TODO:ERROR socket is dead somehow        
            puts("SBN_CLIENT: ERROR CFE_SBN_CLIENT_PIPE_BROKEN_ERR\n");
            return CFE_SBN_CLIENT_PIPE_BROKEN_ERR;
        }
        else if (bytes_received == 0)
        {
            //TODO:ERROR closed remotely 
            puts("SBN_CLIENT: ERROR CFE_SBN_CLIENT_PIPE_CLOSED_ERR\n");
            return CFE_SBN_CLIENT_PIPE_CLOSED_ERR;
        }
        
        total_bytes_recd += bytes_received;
    }
    // 
    // puts("CFE_SBN_CLIENT_ReadBytes THIS MESSAGE:");
    // int i =0;
    // for (i = 0; i < MsgSz; i++)
    // {
    //     printf("0x%02X ", msg_buffer[i]);
    // }
    // printf("\n");
    
    return CFE_SUCCESS;
}


void CFE_SBN_Client_InitPipeTbl(void)
{
    uint8  i;

    for(i = 0; i < CFE_PLATFORM_SBN_CLIENT_MAX_PIPES; i++){
        invalidate_pipe(&PipeTbl[i]);
    }/* end for */
    
    
}

void invalidate_pipe(CFE_SBN_Client_PipeD_t *pipe)
{
    int i;
    
    pipe->InUse         = CFE_SBN_CLIENT_NOT_IN_USE;
    pipe->SysQueueId    = CFE_SBN_CLIENT_UNUSED_QUEUE;
    pipe->PipeId        = CFE_SBN_CLIENT_INVALID_PIPE;
    /* SB always holds one message so Number of messages should always be a minimum of 1 */
    pipe->NumberOfMessages = 1;
    /* Message to be read will be incremented after receive is called */
    /* Therefor initial next message is the last in the chain */
    pipe->ReadMessage = CFE_PLATFORM_SBN_CLIENT_MAX_PIPE_DEPTH - 1;
    memset(&pipe->PipeName[0],0,OS_MAX_API_NAME);
    
    for(i = 0; i < CFE_SBN_CLIENT_MAX_MSG_IDS_PER_PIPE; i++)
    {
        pipe->SubscribedMsgIds[i] = CFE_SBN_CLIENT_INVALID_MSG_ID;
    }
    
    
}

CFE_SB_PipeId_t CFE_SBN_Client_GetAvailPipeIdx(void)
{
    uint8 i;

    /* search for next available pipe entry */
    for(i = 0; i < CFE_PLATFORM_SBN_CLIENT_MAX_PIPES; i++)
    {
        if(PipeTbl[i].InUse == CFE_SBN_CLIENT_NOT_IN_USE){
            return i;
        }/* end if */

    }/* end for */

    return CFE_SBN_CLIENT_INVALID_PIPE;
}

size_t write_message(char *buffer, size_t size)
{
  size_t result;
  
  //printf("Writing Message from SBN_CLIENT sockfd =%d, size = %d\n", sockfd, size);
  
  result = write(sockfd, buffer, size);
  
  //printf("result = %d\n", result);
  
  return result;
}

//NOTE:using memcpy to move message into pipe. What about pointer passing?
//    :can we only look to msgId then memcpy only that then read directly
//    : into pipe? This could speed things up...
void ingest_app_message(int sockfd, SBN_MsgSz_t MsgSz)
{
    //puts("Ingesting APP message");
    unsigned char msg_buffer[CFE_SB_MAX_SB_MSG_SIZE];
    CFE_SB_MsgId_t MsgId;
    
    int status = CFE_SBN_CLIENT_ReadBytes(sockfd, msg_buffer, MsgSz);
    
    if (status != CFE_SUCCESS)
    {
      printf("CFE_SBN_CLIENT_ReadBytes returned a bad status = %d\n", status);
    }

    MsgId = CFE_SBN_Client_GetMsgId((CFE_SB_MsgPtr_t)msg_buffer);
    
    //TODO: check that msgid is valid - How?
    
    // take mutex
    pthread_mutex_lock(&receive_mutex);
    //puts("\n\nRECEIVED LOCK!\n\n");
    
    // put message into pipe
    
    int i;
    
    for(i = 0; i < CFE_PLATFORM_SBN_CLIENT_MAX_PIPES; i++)
    {    
        if (PipeTbl[i].InUse == CFE_SBN_CLIENT_IN_USE)
        {
            int j;
            
            for(j = 0; j < CFE_SBN_CLIENT_MAX_MSG_IDS_PER_PIPE; j++)
            {
                if (PipeTbl[i].SubscribedMsgIds[j] == MsgId)
                {
                    
                    if (PipeTbl[i].NumberOfMessages == CFE_PLATFORM_SBN_CLIENT_MAX_PIPE_DEPTH)
                    {
                        //TODO: error pipe overflow
                        puts("SBN_CLIENT: ERROR pipe overflow");
                        
                        pthread_mutex_unlock(&receive_mutex);
                        return;
                    }
                    else /* message is put into pipe */
                    {    
                        puts("message received");
                        memcpy(PipeTbl[i].Messages[message_entry_point(PipeTbl[i])], msg_buffer, MsgSz);
                        PipeTbl[i].NumberOfMessages++;
                        
                        pthread_mutex_unlock(&receive_mutex);
                        pthread_cond_signal(&received_condition); /* only a received message should send signal */
                        return;
                    } /* end if */
                    
                }/* end if */
                 
            } /* end for */
            
        } /* end if */
    
    } /* end for */
    
    puts("SBN_CLIENT: ERROR no subscription for this msgid");  
    pthread_mutex_unlock(&receive_mutex);
    return;
}

    
uint8 CFE_SBN_Client_GetPipeIdx(CFE_SB_PipeId_t PipeId)
{
  // Quick check because PipeId should match PipeIdx
    if (PipeTbl[PipeId].PipeId == PipeId && PipeTbl[PipeId].InUse == CFE_SBN_CLIENT_IN_USE)
    {
        return PipeId;
    }
    else
    {
        int i;
    
        for(i=0;i<CFE_PLATFORM_SBN_CLIENT_MAX_PIPES;i++)
        {

            if(PipeTbl[i].PipeId == PipeId && PipeTbl[i].InUse == CFE_SBN_CLIENT_IN_USE)
            {
                return i;
            }/* end if */

        } /* end for */
    
        // Pipe ID not found. TODO: error event? No, lets have caller do that...
        return CFE_SBN_CLIENT_INVALID_PIPE;
    }/* end if */
  
}/* end CFE_SBN_Client_GetPipeIdx */

uint8 CFE_SBN_Client_GetMessageSubscribeIndex(CFE_SB_PipeId_t PipeId)
{
    int i;
    
    for (i = 0; i < CFE_SBN_CLIENT_MAX_MSG_IDS_PER_PIPE; i++)
    {
        if (PipeTbl[PipeId].SubscribedMsgIds[i] == CFE_SBN_CLIENT_INVALID_MSG_ID)
        {
            return i;
        }
    }
    
    return CFE_SBN_CLIENT_MAX_MSG_IDS_MET;
}

int connect_to_server(const char *server_ip, uint16_t server_port)
{
    sleep(5);
    int address_converted, connection;

    // Create an ipv4 TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, CFE_SBN_CLIENT_NO_PROTOCOL);

    // socket error
    if (sockfd < 0)
    {
        switch(errno)
        {
            case EACCES:
            puts("socket err = EACCES");
            break; 
            
            case EAFNOSUPPORT:
            puts("socket err = EAFNOSUPPORT");
            break;  
            
            case EINVAL:
            puts("socket err = EINVAL");
            break; 
            
            case EMFILE:
            puts("socket err = EMFILE");
            break; 
            
            case ENOBUFS:
            puts("socket err = ENOBUFS");
            break; 
            
            case ENOMEM:
            puts("socket err = ENOMEM");
            break; 
            
            case EPROTONOSUPPORT:
            puts("socket err = EPROTONOSUPPORT");
            break;  
            
            default:
            printf("Unknown socket error = %d\n", errno);  
        }
        
        perror("connect_to_server socket error");
        return SERVER_SOCKET_ERROR;
    }
    
    printf("sockfd = %d\n", sockfd);

    memset(&server_address, '0', sizeof(server_address));

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_port);

    address_converted = inet_pton(AF_INET, server_ip, &server_address.sin_addr);
    
    printf("address_converted = %d\n", address_converted);
    // inet_pton can have two separate errors, a value of 1 is success.
    if (address_converted == 0)
    {
        perror("connect_to_server inet_pton 0 error");
        return SERVER_INET_PTON_SRC_ERROR;
    }

    if (address_converted == -1)
    {
        perror("connect_to_server inet_pton -1 error");
        return SERVER_INET_PTON_INVALID_AF_ERROR;
    }

    connection = connect(sockfd, (struct sockaddr *)&server_address,
                         sizeof(server_address));
    
    // connect error
    if (connection < 0)
    {
        switch(errno)
        {
            case EACCES:
            puts("connect err = EACCES");
            break;
            
            case EPERM:
            puts("connect err = EPERM");
            break;
            
            case EADDRINUSE:
            puts("connect err = EADDRINUSE");
            break;
            
            case EADDRNOTAVAIL:
            puts("connect err = EADDRNOTAVAIL");
            break;
            
            case EAFNOSUPPORT:
            puts("connect err = EAFNOSUPPORT");
            break;
            
            case EAGAIN:
            puts("connect err = EAGAIN");
            break;
            
            case EALREADY:
            puts("connect err = EALREADY");
            break;
            
            case EBADF:
            puts("connect err = EBADF");
            break;
            
            case ECONNREFUSED:
            puts("connect err = ECONNREFUSED");
            break;
            
            case EFAULT:
            puts("connect err = EFAULT");
            break;
            
            case EINPROGRESS:
            puts("connect err = EINPROGRESS");
            break;
            
            case EINTR:
            puts("connect err = EINTR");
            break;
            
            case EISCONN:
            puts("connect err = EISCONN");
            break;
            
            case ENETUNREACH:
            puts("connect err = ENETUNREACH");
            break;
            
            case ENOTSOCK:
            puts("connect err = ENOTSOCK");
            break;
            
            case EPROTOTYPE:
            puts("connect err = EPROTOTYPE");
            break;
            
            case ETIMEDOUT:
            puts("connect err = ETIMEDOUT");
            break;        
        }
        
        perror("connect_to_server connect error");
        printf("SERVER_CONNECT_ERROR: Connect failed error: %d\n", connection);
        return SERVER_CONNECT_ERROR;
    }

    return sockfd;
}


// Deal with sending out heartbeat messages
#define SBN_TCP_HEARTBEAT_MSG 0xA0
void *heartbeatMinder(void *vargp)
{
  //puts("heartbeatMinder started");
    while(1) // TODO: check run state?
    {
      //puts("heartbeatMinder running");
      
        if (sockfd != 0)
        {
            //printf("SBN_Client: Sending heartbeat\n");
            send_heartbeat(sockfd);
        }
        
        sleep(3);
    }
    return NULL;
}

// TODO: return value?
int send_heartbeat(int sockfd)
{
    //printf("SBN_Client: Sending a heartbeat\n");
    
    int retval;
    char sbn_header[SBN_PACKED_HDR_SZ] = {0};
    
    Pack_t Pack;
    Pack_Init(&Pack, sbn_header, 0 + SBN_PACKED_HDR_SZ, 0);
    
    Pack_UInt16(&Pack, 0);
    Pack_UInt8(&Pack, SBN_TCP_HEARTBEAT_MSG);
    Pack_UInt32(&Pack, 2);
    
    retval = write(sockfd, sbn_header, sizeof(sbn_header));
    
    return retval;
    //printf("SBN_Client: Did the send work? %d\n", retval);
}


// Pipe creation / subscription

int32 __wrap_CFE_SB_CreatePipe(CFE_SB_PipeId_t *PipeIdPtr, uint16 Depth, const char *PipeName)
{
    //printf("SBN_Client: CreatingPipe\n");
    //SBN_ClientInit();
    /* AppId is static for now */
    
    /* caller name is static for now */
    
    /* name will not require NULL terminator */
    
    /* TODO: determine if semaphore is necessary */
    
    /* TODO: determine if taskId is necessary */
    
    /* set user's pipe id value to 'invalid' for error cases below */
    if(PipeIdPtr != NULL){
        *PipeIdPtr = CFE_SBN_CLIENT_INVALID_PIPE;
    }/* end if */
    
    /* check input parameters */
    if((PipeIdPtr == NULL)||(Depth > CFE_PLATFORM_SBN_CLIENT_MAX_PIPE_DEPTH)||(Depth == 0))
    {
        // TODO: what does this do? CFE_SB.HKTlmMsg.Payload.CreatePipeErrorCounter++;
        // TODO: only if semaphore is necessary! CFE_SB_UnlockSharedData(__func__,__LINE__);
        /* replaced by send event below! CFE_EVS_SendEvent(CFE_SB_CR_PIPE_BAD_ARG_EID,CFE_EVS_EventType_ERROR,CFE_SB.AppId,
          "CreatePipeErr:Bad Input Arg:app=%s,ptr=0x%lx,depth=%d,maxdepth=%d",
                CFE_SB_GetAppTskName(TskId,FullName),(unsigned long)PipeIdPtr,(int)Depth,CFE_PLATFORM_SB_MAX_PIPE_DEPTH);
        //     */    
        // CFE_EVS_SendEvent(CFE_SBN_CLIENT_CR_PIPE_ERR_EID,CFE_EVS_EventType_ERROR,
        //   "CreatePipeErr:Bad Input Arg:app=%s,ptr=0x%lx,depth=%d,maxdepth=%d",
        //   APP_NAME,(unsigned long)PipeIdPtr,(int)Depth,
        //   CFE_PLATFORM_SBN_CLIENT_MAX_PIPE_DEPTH);
        return CFE_SBN_CLIENT_BAD_ARGUMENT;
    }/*end if*/
    
      uint8 i;
    
    for(i=0;i<CFE_PLATFORM_SBN_CLIENT_MAX_PIPES;i++)
    {
      
      if (PipeTbl[i].InUse != CFE_SBN_CLIENT_IN_USE)
      {
        // TODO:Initialize pipe
        PipeTbl[i].InUse = CFE_SBN_CLIENT_IN_USE;
        //PipeTbl[i].SysQueueId = ?
        PipeTbl[i].PipeId = i;
        //PipeTbl[i].QueueDepth = ?
        //PipeTbl[i].AppId = ?
        PipeTbl[i].SendErrors = 0;
        //strcpy(&CFE_SB.PipeTbl[PipeTblIdx].AppName[0],&AppName[0]); TODO: is App name required? will cfs proxy handle it?
        strncpy(&PipeTbl[i].PipeName[0], PipeName, OS_MAX_API_NAME); //TODO: Use different value for size?
        //TODO: init Messages to empty?
        
        *PipeIdPtr = i;
        
        return CFE_SUCCESS;
      }
    }
        
    /* if pipe table is full, send event and return error */
    // CFE_EVS_SendEvent(CFE_SBN_CLIENT_MAX_PIPES_MET_EID,CFE_EVS_EventType_ERROR,
    //   "CreatePipeErr:Max Pipes(%d)In Use.app %s",
    //   CFE_PLATFORM_SBN_CLIENT_MAX_PIPES, APP_NAME);
    
    
    
    return CFE_SBN_CLIENT_MAX_PIPES_MET;
}

int32 __wrap_CFE_SB_DeletePipe(CFE_SB_PipeId_t PipeId)
{
  
    uint8 i;

    for(i = 0; i < CFE_PLATFORM_SBN_CLIENT_MAX_PIPES; i++)
    {
        if (PipeTbl[i].PipeId == PipeId)
        {
            if (PipeTbl[i].InUse == CFE_SBN_CLIENT_IN_USE)
            {
                invalidate_pipe(&PipeTbl[i]);
                // CFE_EVS_SendEvent(CFE_SBN_CLIENT_PIPE_DELETED_EID, 
                // CFE_EVS_EventType_DEBUG, "Pipe Deleted:id %d,owner %s",
                // (int)PipeId, APP_NAME);
                return CFE_SUCCESS;
            }
            else
            {
                //TODO:error
                return -1;
            }
            
        }
        
    }
    
    //TODO: if we get here no pipes matched, error
    
    return -2;
}

/**
 * \brief Sends a local subscription over the wire to a peer.
 *
 * @param[in] SubType Whether this is a subscription or unsubscription.
 * @param[in] MsgID The CCSDS message ID being (un)subscribed.
 * @param[in] QoS The CCSDS quality of service being (un)subscribed.
 * @param[in] Peer The Peer interface
 */
static void SendSubToSbn(int SubType, CFE_SB_MsgId_t MsgID,
    CFE_SB_Qos_t QoS)
{
    char Buf[SBN_PACKED_SUB_SZ] = {0};
    Pack_t Pack;
    Pack_Init(&Pack, Buf, SBN_PACKED_SUB_SZ, 0);
    Pack_UInt16(&Pack, 54);
    Pack_UInt8(&Pack, SubType);
    Pack_UInt32(&Pack, 2);
    Pack_Data(&Pack, SBN_IDENT, SBN_IDENT_LEN);
    Pack_UInt16(&Pack, 1);

    Pack_MsgID(&Pack, MsgID);
    Pack_Data(&Pack, &QoS, sizeof(QoS)); /* 2 uint8's */
    
    // int i = 0;
    // puts("SUB MSG: ");
    // for (i;i < Pack.BufUsed; i++)
    // {
    //   printf("0x%02x ", (unsigned char)Buf[i]);
    // }
    // printf("i = %d\n", i);
    // puts("");
    
    size_t write_result = write_message(Buf, Pack.BufUsed);
    
    if (write_result != Pack.BufUsed)
    {
      //TODO: error
      puts("SBN_CLIENT: ERROR SendSubToSbn!!\n");
    }
    
}/* end SendLocalSubToPeer */

int32 __wrap_CFE_SB_Subscribe(CFE_SB_MsgId_t  MsgId, CFE_SB_PipeId_t PipeId)
{
    uint8 PipeIdx;
    uint8 MsgIdIdx;
    CFE_SB_Qos_t QoS;
  
    /* take semaphore to prevent a task switch during this call NOTE:is this necessary for sbn_client?*/
  
    /* get task id for events NOTE: probably not necessary for sbn_client*/
  
    /* get the callers Application Id  NOTE: we already have this locally*/
  
    /* check that the pipe has been created */
    PipeIdx = CFE_SBN_Client_GetPipeIdx(PipeId);
  
    if (PipeIdx == CFE_SBN_CLIENT_INVALID_PIPE)
    {
      //TODO:Error here
      return CFE_SBN_CLIENT_BAD_ARGUMENT;
    }
  
    /* check that the requestor is the owner of the pipe NOTE: not necessary because there can be only 1 app? */
  
    /* check message id key and scope NOTE: do the same as cfe_sb_api?*/
  
    /* Convert the API MsgId into the SB internal representation MsgKey NOTE: not sure what this does yet*/
  
    /* check for duplicate subscription */  
  
    /* check for multiple subscriptions to same pipe? TODO: not sure how this is done */
  
    /* Get the index to the first available element in the routing table NOTE:how does routing table work? do I need it?*/
  
    MsgIdIdx = CFE_SBN_Client_GetMessageSubscribeIndex(PipeId);
    //printf("MsgIdIdx = %d\n", MsgIdIdx);
    if (MsgIdIdx == CFE_SBN_CLIENT_MAX_MSG_IDS_MET)
    {
        //TODO:Error here
        return CFE_SBN_CLIENT_BAD_ARGUMENT;
    }
    
    PipeTbl[PipeIdx].SubscribedMsgIds[MsgIdIdx] = MsgId;
    
    QoS.Priority = 0x00;
    QoS.Reliability = 0x00;
    
    SendSubToSbn(SBN_SUB_MSG, MsgId, QoS);
    
    return CFE_SUCCESS;
}

int32 __wrap_CFE_SB_SubscribeEx(CFE_SB_MsgId_t  MsgId, CFE_SB_PipeId_t PipeId, CFE_SB_Qos_t Quality, uint16 MsgLim)
{
    printf ("SBN_CLIENT: ERROR CFE_SB_SubscribeEx not yet implemented\n");
    return -1;
}

int32 __wrap_CFE_SB_SubscribeLocal(CFE_SB_MsgId_t  MsgId, CFE_SB_PipeId_t PipeId, uint16 MsgLim)
{
    printf ("SBN_CLIENT: ERROR CFE_SB_SubscribeLocal not yet implemented\n");
    return -1;
}

int32 __wrap_CFE_SB_Unsubscribe(CFE_SB_MsgId_t  MsgId, CFE_SB_PipeId_t PipeId)
{
    printf ("SBN_CLIENT: ERROR CFE_SB_Unsubscribe not yet implemented\n");
    return -1;
}

int32 __wrap_CFE_SB_UnsubscribeLocal(CFE_SB_MsgId_t  MsgId, CFE_SB_PipeId_t PipeId)
{
    printf ("SBN_CLIENT: ERROR CFE_SB_UnsubscribeLocal not yet implemented\n");
    return -1;
}

uint32 __wrap_CFE_SB_SendMsg(CFE_SB_Msg_t *msg)
{
    //printf("SBN_Client:Sending Message...\n");
    char *buffer;
    uint16 msg_size = CFE_SBN_Client_GetTotalMsgLength(msg);

    size_t write_result, total_size = msg_size + SBN_PACKED_HDR_SZ;
    Pack_t Pack;

    if (total_size > CFE_SB_MAX_SB_MSG_SIZE)
    {
        return CFE_SB_MSG_TOO_BIG;
    }

    buffer = malloc(total_size);

    Pack_Init(&Pack, buffer, total_size, 0);

    Pack_UInt16(&Pack, msg_size);
    Pack_UInt8(&Pack, SBN_APP_MSG);
    Pack_UInt32(&Pack, cpuId);

    memcpy(buffer + SBN_PACKED_HDR_SZ, msg, msg_size);
    
    // int i = 0;
    // puts("SUB MSG: ");
    // for (i;i < Pack.BufUsed; i++)
    // {
    //   printf("0x%02x ", (unsigned char)Buf[i]);
    // }
    // printf("i = %d\n", i);
    // puts("");

    write_result = write_message(buffer, total_size);

    if (write_result != total_size)
    {
        // TODO: This isn't an allocation error...
        return CFE_SB_BUF_ALOC_ERR;
    }

    free(buffer);

    return CFE_SUCCESS;
}

int32 __wrap_CFE_SB_RcvMsg(CFE_SB_MsgPtr_t *BufPtr, CFE_SB_PipeId_t PipeId, int32 TimeOut)
{
    //puts("SBN_CLIENT: Checking for messages...");
    // Oh my.
    // Need to have multiple pipes... so the subscribe thing
    // Need to coordinate with the recv_msg thread... so locking?
    // Also, what about messages that get split? Is that an issue?
    int8   pipe_idx;
    struct timespec enter_time;
    int32  status;
    
    clock_gettime(CLOCK_REALTIME, &enter_time);
      
    pipe_idx = CFE_SBN_Client_GetPipeIdx(PipeId);
    
    if (pipe_idx == CFE_SBN_CLIENT_INVALID_PIPE)
    {
        puts("SBN_CLIENT: ERROR INVALID PIPE ERROR!");
        //TODO: don't know if this is a valid error return value;
        status = CFE_SBN_CLIENT_INVALID_PIPE;
    }
    else
    {
        CFE_SBN_Client_PipeD_t *pipe = &PipeTbl[pipe_idx];
        
        pthread_mutex_lock(&receive_mutex);
        //puts("\nLOCKING\n");
        
        if (pipe->NumberOfMessages > 1)
        {
            /* must progress to next message in pipe */
            uint32 next_msg = (pipe->ReadMessage + 1) % CFE_PLATFORM_SBN_CLIENT_MAX_PIPE_DEPTH;
            pipe->ReadMessage = next_msg;
            
            *BufPtr = (CFE_SB_MsgPtr_t)(&(pipe->Messages[next_msg]));
            
            pipe->NumberOfMessages -= 1;
            status = CFE_SUCCESS;
        }
        else
        {
          int wait_result;
          struct timespec future_timeout;
          
          /* set future time for timeout check to entry time + timeout milliseconds */
          future_timeout.tv_sec = enter_time.tv_sec;
          future_timeout.tv_nsec = enter_time.tv_nsec + (TimeOut * pow(10, 6));

          /* when nsec greater than 1 second perform update to seconds and nanoseconds */
          if (future_timeout.tv_nsec >= pow(10, 9))
          {
            future_timeout.tv_sec += future_timeout.tv_nsec / pow(10, 9);
            future_timeout.tv_nsec = future_timeout.tv_nsec % (long) pow(10, 9);
          }
          
          wait_result = pthread_cond_timedwait(&received_condition, &receive_mutex, &future_timeout);
               
          if (wait_result == ETIMEDOUT)
          {
            //puts("\n\nTIMEOUT!!!!!\n\n");
            status = CFE_EVS_EventType_ERROR;
          } /* end if */
          
        } /* end if */
    
    } /* end if */
    
    int pmu = pthread_mutex_unlock(&receive_mutex);
    
    if (pmu != 0)
    {
      status =  CFE_EVS_ERROR;
    } /* end if */
    
    return status;
} /* end __wrap_CFE_SB_RcvMsg */

int32 __wrap_CFE_SB_ZeroCopySend(CFE_SB_Msg_t *MsgPtr, CFE_SB_ZeroCopyHandle_t BufferHandle)
{
    printf ("SBN_CLIENT: ERROR CFE_SB_ZeroCopySend not yet implemented\n");
    return -1;
}
// Receiving messages

void *receiveMinder(void *vargp)
{
  //puts("receiveMinder started");
    while(1) // TODO: check run state?
    {
        //printf("SBN_Client: Checking messages\n");
        recv_msg(sockfd); // TODO: pass message pointer?
        // On heartbeats, need to update known liveness state of SBN
        // On other messages, need to make available for next CFE_SB_RcvMsg call
    }
}

int recv_msg(int sockfd)
{
    unsigned char sbn_hdr_buffer[SBN_PACKED_HDR_SZ];
    unsigned char msg[CFE_SB_MAX_SB_MSG_SIZE];
    SBN_MsgSz_t MsgSz;
    SBN_MsgType_t MsgType;
    SBN_CpuID_t CpuID;
    
    int status = CFE_SBN_CLIENT_ReadBytes(sockfd, sbn_hdr_buffer, SBN_PACKED_HDR_SZ);
    
    //TODO: status check goes here
    
    if (status != CFE_SUCCESS)
    {
      printf("SBN_CLIENT: recv_msg call to CFE_SBN_CLIENT_ReadBytes returned status = %d\n", status);
    }

    // TODO: error checking (-1 returned, perror)

    Unpack_t Unpack;
    Unpack_Init(&Unpack, sbn_hdr_buffer, SBN_PACKED_HDR_SZ);
    Unpack_UInt16(&Unpack, &MsgSz);
    Unpack_UInt8(&Unpack, &MsgType);
    Unpack_UInt32(&Unpack, &CpuID);
    
    // puts("RECEIVEING THIS MESSAGE:");
    // int i =0;
    // for (i = 0; i < 50 + SBN_PACKED_HDR_SZ; i++)
    // {
    //     printf("0x%02X ", sbn_hdr_buffer[i]);
    // }
    // printf("\n");

    printf("Msg Size: %d\t Msg Type: %d\t Processor_ID: %d\n", MsgSz, MsgType, CpuID);

    //TODO: check cpuID to see if it is correct for this location?

    switch(MsgType)
    {
        case SBN_NO_MSG:
            status = CFE_SBN_CLIENT_ReadBytes(sockfd, msg, MsgSz);
            break;
        case SBN_SUB_MSG:
            status = CFE_SBN_CLIENT_ReadBytes(sockfd, msg, MsgSz);
            break;
        case SBN_UNSUB_MSG:
            status = CFE_SBN_CLIENT_ReadBytes(sockfd, msg, MsgSz);
            break;
        case SBN_APP_MSG:
            ingest_app_message(sockfd, MsgSz);
            status = CFE_SUCCESS;
            break;
        case SBN_PROTO_MSG:      
            status = CFE_SBN_CLIENT_ReadBytes(sockfd, msg, MsgSz);
            break;
        case SBN_RECVD_HEARTBEAT_MSG:
            status = CFE_SBN_CLIENT_ReadBytes(sockfd, msg, MsgSz);
            break;

        default:
            printf("SBN_CLIENT: ERROR - recv_msg unrecognized type %d\n", MsgType);
            status =  CFE_EVS_ERROR; //TODO: change error
    }
    
    // puts("Received Message:");
    // for (int i = 0; i < MsgSz; i++)
    // {
    //     printf("0x%02X ", msg[i]);
    // }
    // printf("\n");
    
    return status;
}

CFE_SB_MsgId_t CFE_SBN_Client_GetMsgId(CFE_SB_MsgPtr_t MsgPtr)
{
   CFE_SB_MsgId_t MsgId = 0;

#ifdef MESSAGE_FORMAT_IS_CCSDS

#ifndef MESSAGE_FORMAT_IS_CCSDS_VER_2  
    MsgId = CCSDS_RD_SID(MsgPtr->Hdr);
#else

    uint32            SubSystemId;

    MsgId = CCSDS_RD_APID(MsgPtr->Hdr); /* Primary header APID  */
     
    if ( CCSDS_RD_TYPE(MsgPtr->Hdr) == CCSDS_CMD)
      MsgId = MsgId | CFE_SB_CMD_MESSAGE_TYPE;  

    /* Add in the SubSystem ID as needed */
    SubSystemId = CCSDS_RD_SUBSYSTEM_ID(MsgPtr->SpacePacket.ApidQ);
    MsgId = (MsgId | (SubSystemId << 8));

#endif
#endif

return MsgId;

}/* end CFE_SBN_Client_GetMsgId */

uint16 CFE_SBN_Client_GetTotalMsgLength(CFE_SB_MsgPtr_t MsgPtr)
{
#ifdef MESSAGE_FORMAT_IS_CCSDS

    return CCSDS_RD_LEN(MsgPtr->Hdr);

#endif
}

