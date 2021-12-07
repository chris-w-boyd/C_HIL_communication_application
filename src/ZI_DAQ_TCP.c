#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#define PRsize_t "z"
#define PRptrdiff_t "t"
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>   //timer functions 
#include <signal.h> //break inf loop with Ctrl+C

#include <sys/types.h> //Network Transfer 
#include <sys/socket.h> //Network Transfer 
#include <netinet/in.h> //Network Transfer 
#include <netdb.h> //Network Transfer 
#include <pthread.h> //Multi-threaded programming
 
#include "ziAPI.h" //Zurich Instruments API

/*
 * 
 *  This version of the program is multi-threaded and can send data between
 *  the Zurich Instruments and the OPAL-RT in both directions. 
 *
 */

static inline void checkError(ZIResult_enum resultCode, const char* desc){
	if(resultCode != ZI_INFO_SUCCESS){
		char* message;
		ziAPIGetError(resultCode, &message, NULL);
		fprintf(stderr,"%s, Error Code: %s\n",desc,message);
	}
}

static inline bool isError(ZIResult_enum resultCode, const char* desc) {
  if (resultCode != ZI_INFO_SUCCESS) {
    char* message;
    ziAPIGetError(resultCode, &message, NULL);
    fprintf(stderr,"%s, Error Code: %s\n",desc,message);
    return true;
  }
  return false;
}

volatile sig_atomic_t cleanup_exit = 0;

void sigintHandler(int sig_num){
	cleanup_exit = 1;
}

double timeSince(clock_t t1);

void readData(ZIConnection* conn_ptr,ZIModuleHandle* DAQhdl_ptr, char *demod_sig_paths[], ZIModuleEventPtr* ev_ptr, char *path, int *sockfd_ptr, double **demodData,double **timeData,double clockbase_d, FILE *out_file);

void connectServer(const char* hostname, int portno, void *p_arg);

static int readCount = 0; //keep track of number of calls to ziAPIModRead() 

static ZITimeStamp timeStamp0 = 0; //log the first timestamp

static int *max_chunks_ptr; 

static int *print_info_ptr;

static void initialize_cfg(void *p_arg, int argc, char *argv[]);

static void connect_to_HF2LI(void *p_arg);

//Threads
void *send_to_ZI(void *p_arg);
void *recv_from_ZI(void *p_arg); 


//===============================================================================================================//
//Define the structure for passing data between the send_to_ZI and recv_from_ZI threads
typedef struct data_in_out
{
  ZIConnection conn; 
  ZIResult_enum retVal;
  char *errBuffer;
  char *serverAddress;
  ZIIntegerData clockbase;
  double clockbase_d; 
  int daqType; 
  double demod_sampling_rate;
  double des_module_sampling_rate;
  int gridMode;
  double des_burst_duration;
  bool endless;
  double total_duration;
  FILE *out_file; 
  int num_cols;
  int num_bursts;
  int num_rows;
  int sock_id;
  int sock_id_data;

} ZIDAQCfgStruct; 


//========================================MAIN========================================================================

int main(int argc, char *argv[]){

		printf("MAIN thread starting\n");

		pthread_t tid_send_to_ZI;
		pthread_t tid_recv_from_ZI;
		pthread_attr_t attr_send;
		pthread_attr_t attr_recv;
		int err;
		
		ZIDAQCfgStruct *ZIDAQCfg;
		
		signal(SIGINT, sigintHandler); //assign a signal handler to SIGINT (Ctrl+C) 
		
		//check for proper arguments to the program
		if(argc != 9){
		   printf("Incorrect number of arguments\n"); 
		   printf("Usage: [demod_rate (Hz)] [des_module_samp_rate (Hz)] [grid_mode (NN|LIN|EX)] [burst_duration(s)] [endless(true|false)] [total_duration(s)] [max_chunks (int)] [print_info (0|1)] \n");
		   return(1);
		}
		
		ZIDAQCfg = (ZIDAQCfgStruct *)malloc(sizeof(ZIDAQCfgStruct));
		
		if(ZIDAQCfg == NULL){
		   printf("Error: Failed to allocate configuration structure\n");
		   exit(-1); 
		}
		
		//parse inputs into the ZIDAQ Config Struct
		initialize_cfg(ZIDAQCfg,argc,argv);
		
		//connect to and initialize HF2LI 
		connect_to_HF2LI(ZIDAQCfg);
		
		//Initialize TCP session
		//================ SETUP TCP/IP SOCKETS ================================================//
		connectServer("192.168.0.1",7220,ZIDAQCfg);
		
		//start thread to receive demod data from ZI
		pthread_attr_init (&attr_recv);
		if((pthread_create (&tid_recv_from_ZI, &attr_recv, recv_from_ZI, ZIDAQCfg)) != 0){
			
			printf("Error: cannot create thread (recv_from_ZI)\n");
			exit(-1);
			
		}
		
		//start thread to send control data to ZI
		pthread_attr_init(&attr_send);
		if((pthread_create(&tid_send_to_ZI, &attr_send, send_to_ZI, ZIDAQCfg)) != 0){
			
			printf("Error: cannot create thread (recv_from_ZI)\n");
			exit(-1);
			
		}
		
		//wait for receive_from_ZI thread to finish
		if ((err = pthread_join(tid_recv_from_ZI, NULL)) != 0)
		{
		    printf("Error: pthread_join (recv_from_ZI), errno = %d\n",err);
		}
		
		//wait for send_to_ZI thread to finish 
		if ((err = pthread_join(tid_send_to_ZI, NULL)) != 0)
		{
		    printf("Error: pthread_join (send_to_ZI), errno = %d\n",err);
		}
		
		//cleanup + deallocate memory 
		free(max_chunks_ptr); // free memory associated with global pointers
		free(print_info_ptr);
		
	    // Disconnect from the Data Server. Since ZIAPIDisconnect always returns
		// ZI_INFO_SUCCESS no error handling is required.
		ziAPIDisconnect(ZIDAQCfg->conn);
        
        // Destroy the ZIConnection. Since ZIAPIDestroy always returns
        // ZI_INFO_SUCCESS, no error handling is required.
        ziAPIDestroy(ZIDAQCfg->conn);
        
        printf("MAIN thread ending\n");
		
		return 0; // end of main 
}		

//=========================== HELPER FUNCTION DEFINITIONS ============================================================
/*=================================================================
 * Name:   recv_from_ZI
 * Description: read data from ZI and send to OPAL-RT over TCP/IP
 * Argument: p_arg Pointer to configuration structure
 * Returns: none
 * Caller(s): main()
 * ================================================================
 */
void *recv_from_ZI(void *p_arg){
	
	 ZIDAQCfgStruct *p_cfg = (ZIDAQCfgStruct *) p_arg;
	 ZIConnection conn_recv;
	 ZIModuleHandle DAQhdl;
     ZIResult_enum retVal_recv;
     char *errBuffer_recv;
     
     printf("Receive from ZI thread starting\n");
     
     // Initialize ZIConnection.
    checkError(ziAPIInit(&conn_recv),"Can't Init Connection");
      
    // Connect to the Data Server: Use port 8005 for the HF2 Data Server
    //HF2 only support ZI_API_VERSION_1, see the LabOne Programming Manual
    // for an explanation of API Levels.
    
    retVal_recv = ziAPIConnectEx(conn_recv, p_cfg->serverAddress, 8005, ZI_API_VERSION_1, NULL);
        if (retVal_recv != ZI_INFO_SUCCESS) {
                ziAPIGetError(retVal_recv, &(errBuffer_recv), NULL);
                fprintf(stderr, "Error, can't connect to the Data Server: `%s`.\n", errBuffer_recv);
                exit(-1);
        }
        
     //=========================== SETUP THE DAQ Module ===================================//
	 //Create DAQ Module
	 checkError(ziAPIModCreate(conn_recv,(&DAQhdl),"dataAcquisitionModule"),"Cant' create DAQ module");
			
	 //set the device serial using ziAPIModSetString
	 const char devName[] = "dev1418"; // "dev1418" or"dev447"   
	 checkError(ziAPIModSetString(conn_recv,DAQhdl,"dataAcquisitionModule/device",devName),"Can't set dev serial for DAQ");
	 
	 //Set DAQ Module Acquisition Type 
	 checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/type",p_cfg->daqType),"Can't set DAQ type");
		
	 //Set the DAQ Module Grid Mode to 1 = nearest neighbor, 2 = linear interpolation, 4 = exact mode
	 //The grid/mode parameter specifies how the data is sampled onto the time, respectively frequency grid
	 checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/grid/mode",p_cfg->gridMode),"Can't set DAQ grid mode");
	 
	 //Set endless triggering parameter 
	 checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/endless",p_cfg->endless),"Can't set endless param");
	 if(!p_cfg->endless){
		 //if we are not using endless trigger we need to specify how many burst_durations to record
		 checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/count",p_cfg->num_bursts),"Can't set count param");
	 }
	 
	 if(p_cfg->gridMode == 4){
			
		//For exact mode set the /grid/cols parameter: # of columns returned in the data grid (matrix) 
		//The data along the horizontal axis is resampled to the # of samples defined by grid/cols
		checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/grid/cols",p_cfg->num_cols),"Can't set DAQ grid/cols");		
		
	 }else if((p_cfg->gridMode == 1) || (p_cfg->gridMode == 2)){ //grid mode is nearest neighbor or linear interpolation
		checkError(ziAPIModSetDoubleData(conn_recv,DAQhdl,"dataAcquisitionModule/duration",p_cfg->des_burst_duration),"Can't set DAQ duration");
			
		checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/grid/cols",p_cfg->num_cols),"Can't set DAQ grid/cols");
	 }else{
		 fprintf(stderr, "Invalid grid mode specified\n");
		 exit(-1);
	 }
	 
	 
	 //Subscribe to nodes of interest
	 char *demod_sig_paths[4];
	 demod_sig_paths[0] = "/dev1418/demods/0/sample.x";
	 demod_sig_paths[1] = "/dev1418/demods/0/sample.y";
	 demod_sig_paths[2] = "/dev1418/demods/3/sample.x";
	 demod_sig_paths[3] = "/dev1418/demods/3/sample.y";
		
	 checkError(ziAPIModSubscribe(conn_recv,DAQhdl,demod_sig_paths[0]),"Can't subscribe to demod");
	 checkError(ziAPIModSubscribe(conn_recv,DAQhdl,demod_sig_paths[1]),"Can't subscribe to demod");
	 checkError(ziAPIModSubscribe(conn_recv,DAQhdl,demod_sig_paths[2]),"Can't subscribe to demod");
	 checkError(ziAPIModSubscribe(conn_recv,DAQhdl,demod_sig_paths[3]),"Can't subscribe to demod");
	 
	 //========Execute DAQ Module and Retrieve Data in an Infinite Loop =====================//		
	 
	 ZIIntegerData finished;
	 ZIModuleEventPtr ev = NULL;
	 char path[1024]; // [out] node path that the data is coming from
	 double t_update = 0.9*p_cfg->des_burst_duration; //maximum time to wait before reading out new data	
	 
	 double *demodData[p_cfg->num_rows];
	 double *timeData[p_cfg->num_rows]; 
		for( int i = 0; i < p_cfg->num_rows; i++){
			demodData[i] = (double *)malloc(p_cfg->num_cols*sizeof(double));
			timeData[i] = (double *)malloc(p_cfg->num_cols*sizeof(double));
			if((demodData[i] == NULL) || (timeData[i] == NULL)){
				printf("Can't initialize memory for demodData\n");
				exit(0);
			}
		}
	 
	 //double demodData[NUM_ROWS][NUM_COLS]; // row0: X (chunk 1), row1: X (chunk2), row2: Y (chunk1), row3 Y (chunk2) 
	 //double timeData[NUM_ROWS][NUM_COLS]; // "        " 
	 
	 //Execute DAQ Module
	 checkError(ziAPIModSetIntegerData(conn_recv,DAQhdl,"dataAcquisitionModule/enable",1),"Can't start module");
	 sleep(1); //sleep long enough for data buffer to fill up initially
 
	 checkError(ziAPIModFinished(conn_recv,DAQhdl, &finished),"Can't check if module finished");
	 ZIIntegerData loop_cond = (p_cfg->endless ? 0 : finished);
	 
	 double t_elapsed; 
	 double t_diff;
	 unsigned int t_diff_us;
	 clock_t t0_loop; 
	 
	 while(!loop_cond){ //(Loop 1)Call the read function in a loop 
				
			t0_loop = clock();
			
			
			
			// call readData		
			readData((&conn_recv),(&DAQhdl), demod_sig_paths, (&ev), path,(&(p_cfg->sock_id_data)),demodData,timeData,p_cfg->clockbase_d,p_cfg->out_file);
			
			//at most sleep only t_update seconds since last read
			t_elapsed = timeSince(t0_loop);
			t_diff = (t_update - t_elapsed );
			if(*print_info_ptr){
				//printf("readData took: %f seconds to run, Will wait %f seconds until next Read() \n",t_elapsed,t_diff);
				fprintf(p_cfg->out_file,"readData took: %f seconds to run, Will wait %f seconds until next Read() \n",t_elapsed,t_diff);
			}
			if(t_diff < 0){
				t_diff = 0; 
			}
			t_diff_us = (unsigned int) (t_diff*1e6);
			usleep(t_diff_us);
					
			checkError(ziAPIModFinished(conn_recv,DAQhdl, &loop_cond),"Can't check if module finished");
			 
			 if(cleanup_exit){
				printf("\nCtrl+C pressed: cleaning up and exiting\n");
				break;  
			 }	
			 
		}
		
		if(!cleanup_exit){
		//there may be new data between the last read and calling finished()  (THINK ABOUT THIS!)
		readData((&conn_recv),(&DAQhdl), demod_sig_paths, (&ev), path, (&(p_cfg->sock_id_data)),demodData,timeData,p_cfg->clockbase_d,p_cfg->out_file);
		}
		
		//clean up all connections and memory allocated in recv_from_ZI thread
		for( int i = 0; i < p_cfg->num_rows; i++){
			free(demodData[i]); 
			free(timeData[i]);
		}
		
		checkError(ziAPIModEventDeallocate(conn_recv,DAQhdl,ev),"Can't deallocate event ptr");
		checkError(ziAPIModFinish(conn_recv,DAQhdl),"Can't stop DAQ module");
								
	    //Terminate DAQ module thread and destroy the module
		checkError(ziAPIModClear(conn_recv,DAQhdl),"Can't terminate DAQ module");
		
		// Disconnect from the Data Server. Since ZIAPIDisconnect always returns
		// ZI_INFO_SUCCESS no error handling is required.
		ziAPIDisconnect(conn_recv);
        
        // Destroy the ZIConnection. Since ZIAPIDestroy always returns
        // ZI_INFO_SUCCESS, no error handling is required.
        ziAPIDestroy(conn_recv);
        
        printf("Receive from ZI thread ending\n");
        
        return ((void *)0);
		
}
//--------------------------------------------------------------------------------------------------------------------
/*=================================================================
 * Name:   send_to_ZI
 * Description: send data from OPAL-RT (via TCP/IP) to ZI
 * Argument: p_arg Pointer to configuration structure
 * Returns: none
 * Caller(s): main()
 * ================================================================
 */
void *send_to_ZI(void *p_arg){
	
	ZIDAQCfgStruct *p_cfg = (ZIDAQCfgStruct *) p_arg;
	ZIConnection conn_send;
    ZIResult_enum retVal_send;
    char *errBuffer_send;
	double value; 
	double sigout_range = 1.0;
	
	printf("Send to ZI thread starting\n");
	
    // Initialize ZIConnection.
    checkError(ziAPIInit(&conn_send),"Can't Init Connection");
      
    // Connect to the Data Server: Use port 8005 for the HF2 Data Server
    //HF2 only support ZI_API_VERSION_1, see the LabOne Programming Manual
    // for an explanation of API Levels.
    
    retVal_send = ziAPIConnectEx(conn_send, p_cfg->serverAddress, 8005, ZI_API_VERSION_1, NULL);
        if (retVal_send != ZI_INFO_SUCCESS){
                ziAPIGetError(retVal_send, &(errBuffer_send), NULL);
                fprintf(stderr, "Error, can't connect to the Data Server: `%s`.\n", errBuffer_send);
                exit(-1);
        }


     
     
     
     do{
	    //read data from OPAL-RT
	    if( read(p_cfg->sock_id_data,&value,sizeof(value)) < 0){
			printf("Error reading data from OPAL-RT\n");
		}
		//set the amplitude
		printf("value = %f\n",value);
		checkError(ziAPISetValueD(conn_send, "/dev1418/sigouts/0/amplitudes/6", (value/sigout_range)),"Can't set sigout amplitude");
		usleep(100);	 
	 }while(!cleanup_exit);
     
     //Cleanup 
     
     // Disconnect from the Data Server. Since ZIAPIDisconnect always returns
	 // ZI_INFO_SUCCESS no error handling is required.
	 ziAPIDisconnect(conn_send);
        
     // Destroy the ZIConnection. Since ZIAPIDestroy always returns
     // ZI_INFO_SUCCESS, no error handling is required.
     ziAPIDestroy(conn_send);
     
     printf("Send to ZI thread ending\n");
        
     return ((void *)0);     
}
//---------------------------------------------------------------------------------------------------------------------//

void readData(ZIConnection* conn_ptr,ZIModuleHandle* DAQhdl_ptr, char *demod_sig_paths[], ZIModuleEventPtr* ev_ptr, char *path, int *sockfd_ptr, double **demodData,double **timeData, double clockbase_d, FILE *out_file){  

	 ZIValueType_enum valueType; // [out] ZIValue_type_enum of the node's data
	 uint64_t chunks; // [out] number of data chunks available from node
	 bool path_match[4]; // [0] = demod1 X, [1] = demod1 Y, [2] = demod4 X, [3] = demod4 Y
	 int row_ind; 
	 double XYquartet[4]; // [0] = demod1 X, [1] = demod1 Y, [2] = demod4 X, [3] = demod4 Y
	 
	 //make currently accumulated data available for read in C program 
	 if(isError(ziAPIModRead((*conn_ptr),(*DAQhdl_ptr),NULL),"Can't initiate read")){   
			char errBuf[1024] = "ziAPIGetLastError was not successful";
			checkError(ziAPIGetLastError((*conn_ptr),errBuf,1024),"Can't get last error");
			fprintf(stderr, "Error: `%s`.\n", errBuf);
			exit(-1); 
	  }
	  
	  readCount++; //increment counter for number of calls to readData()
	  if(*print_info_ptr){
		//printf("Read #%d\n",readCount);
		fprintf(out_file,"Read #%d\n",readCount);
	  }
	  
	  ZIResult_enum res = ziAPIModNextNode((*conn_ptr), (*DAQhdl_ptr), path, 1024, &valueType, &chunks);
	  //(Loop 2) iterate through all subscribed nodes
	  while(res == ZI_INFO_SUCCESS){
				
					   
		      	//only process the subscribed demodulator signals
		      	 path_match[0] = (!strcmp(path,demod_sig_paths[0]));
		      	 path_match[1] = (!strcmp(path,demod_sig_paths[1]));
		      	 path_match[2] = (!strcmp(path,demod_sig_paths[2]));
		      	 path_match[3] = (!strcmp(path,demod_sig_paths[3]));
		      	 
				if(path_match[0] || path_match[1] || path_match[2] || path_match[3]){
					
					if(path_match[0]){  // comment out for real operation 
						printf("Read #%d Num Chunks: %" PRIu64 "\n",readCount,chunks);   
					}
					
					if(*print_info_ptr){
						//printf("Got node: %s with %" PRIu64 " chunks of type %d\n", path, chunks, valueType);
						fprintf(out_file,"Got node: %s with %" PRIu64 " chunks of type %d\n", path, chunks, valueType); 
					}
					
					if(chunks > (*max_chunks_ptr)){
						printf("Chunks received (%" PRIu64 ") exceeds max chunks allowed (%d)!\n",chunks,(*max_chunks_ptr));
						if(*print_info_ptr){
							fprintf(out_file,"Chunks received (%" PRIu64 ") exceeds max chunks allowed (%d)!\n",chunks,(*max_chunks_ptr));
						}
						exit(0); 
					}
					   
					//(Loop 3) iterate through all available chunks for each signal node
					for(uint64_t chunk = 0; chunk < chunks; ++chunk){
							
						checkError(ziAPIModGetChunk((*conn_ptr),(*DAQhdl_ptr),chunk, ev_ptr),"Can't get chunk");
						if(*print_info_ptr){
							fprintf(out_file,"Data of chunk %" PRIu64 ": type %d, header time %" PRId64 "\n", chunk, (*ev_ptr)->value->valueType, (*ev_ptr)->header->systemTime);
							fprintf(out_file,"Number of samples in this chunk: %d\n", (*ev_ptr)->value->count);
						}
						
						if(timeStamp0 == 0){ //set the initial time stamp on first chunk that is returned 
						   timeStamp0 = (*ev_ptr)->value->value.doubleDataTS[0].timeStamp;
						   if(*print_info_ptr){
							 fprintf(out_file,"Time Stamp0: %" PRIu64 "\n",timeStamp0);	
						   }
						}
						
						if(path_match[0]){ 
						   row_ind = chunk + 0;
						} else if(path_match[1]){
						   row_ind = chunk + (*max_chunks_ptr);
						} else if(path_match[2]){
							row_ind = chunk + (2*(*max_chunks_ptr));
						} else if(path_match[3]){
							row_ind = chunk + (3*(*max_chunks_ptr));
						}
						
						//(Loop 4) iterate through num_col samples
						
						for( int i = 0; i < (*ev_ptr)->value->count; ++i){
							demodData[row_ind][i] = (*ev_ptr)->value->value.doubleDataTS[i].value;
							//timeData[row_ind][i] = (((*ev_ptr)->value->value.doubleDataTS[i].timeStamp) - timeStamp0)/clockbase_d;
						} 
							
					}
										  
					if(path_match[3]){
						
						//we've collected all the data, now send it to remote host
						for(uint64_t chunk = 0; chunk < chunks; ++chunk){
							for( int i = 0; i < (*ev_ptr)->value->count; ++i){
								XYquartet[0] = demodData[chunk][i];  
								XYquartet[1] = demodData[(chunk+(*max_chunks_ptr))][i];
								XYquartet[2] = demodData[(chunk+(2*(*max_chunks_ptr)))][i];
								XYquartet[3] = demodData[(chunk+(3*(*max_chunks_ptr)))][i]; 
								if(write((*sockfd_ptr),(XYquartet),sizeof(XYquartet)) < 0)
								{
									fprintf(stderr,"ERROR sending data to server\n");
									exit(0);
								}
								//fprintf(out_file,"%f,%e,%f,%e\n",timeData[chunk][i],demodData[chunk][i],timeData[chunk+2][i],demodData[chunk+2][i]);
							} 
					   }
					 
						//stop iterating through the nodes after last node of interest is processed
						break;
					}
					  
				}else{
					   if(*print_info_ptr){ 
							fprintf(out_file,"Got node: %s with %" PRIu64 " chunks of type %d\n", path, chunks, valueType);					
						}
				}
		   	   
				res = ziAPIModNextNode((*conn_ptr), (*DAQhdl_ptr), path, 1024, &valueType, &chunks);		
	  }
	  if((res != ZI_WARNING_NOTFOUND) && (res != ZI_INFO_SUCCESS) ){
			checkError(res,"Error in NODE LOOPING\n");
	  }//end function
	  
	  return;
}
//-----------------------------------------------------------------------------------------------------------------//
void connectServer(const char *hostname, int portno, void *p_arg){
		
			ZIDAQCfgStruct *p_cfg = (ZIDAQCfgStruct *) p_arg;
			struct sockaddr_in serv_addr;
			struct hostent *server;
		
		
			p_cfg->sock_id = socket(AF_INET, SOCK_STREAM, 0);
			if(p_cfg->sock_id < 0)
			{
				fprintf(stderr,"ERROR creating socket\n");
				exit(0);
			}
		
			server = gethostbyname(hostname);
			if( server == NULL)
			{
				fprintf(stderr,"ERROR, no such host\n");
				exit(0);
			}
	
			bzero((char *) &serv_addr, sizeof(serv_addr));
			serv_addr.sin_family = AF_INET;
	
			bcopy((char *)server->h_addr,(char *)&serv_addr.sin_addr.s_addr,server->h_length);
			serv_addr.sin_port = htons(portno);
	
			//const struct sockaddr *p_serv_addr = (struct sockaddr *) (&serv_addr);
			
	
			if( connect(p_cfg->sock_id,(struct sockaddr *) (&serv_addr),sizeof(serv_addr)) < 0 )
			{
				fprintf(stderr,"ERROR connecting to server\n");
				exit(0);
			}
			
			p_cfg->sock_id_data = p_cfg->sock_id;
			
		    return; 
}
//------------------------------------------------------------------------------------------------------------------//
static void initialize_cfg(void *p_arg, int argc, char *argv[]){
	//initialize the Config struct based on the command line argument

	ZIDAQCfgStruct *p_cfg = (ZIDAQCfgStruct *) p_arg;
	
	p_cfg->daqType = 0; // continuous = 0, PARAMETER: tpye
	
	p_cfg->demod_sampling_rate = atof(argv[1]);
	
	p_cfg->des_module_sampling_rate = atof(argv[2]); //number of points/second (in exact mode set to highest rate of all subscribed paths)
	
    
    if((p_cfg->demod_sampling_rate == 0.0) || (p_cfg->des_module_sampling_rate == 0.0)){
			printf("invalid sampling rate set\n");
			exit(-1);
	}
        
    // PARAMETER: grid/mode ,1 = nearest neighbor, 2 = linear interpolation, 4 = exact mode
    //In exact mode duration is auto computed as ("grid/cols"/(max(sampling rates of subscribed data)))
        
    if(!strcmp(argv[3],"NN")){
		p_cfg->gridMode = 1;
	}else if(!strcmp(argv[3],"LIN")){
		p_cfg->gridMode = 2;
	}else if(!strcmp(argv[3],"EX")){
		p_cfg->gridMode = 4;
	}else{
		printf("invalid grid mode specified\n");
		exit(-1);
	}
        
    p_cfg->des_burst_duration = atof(argv[4]); //PARAMETER: duration, SET DIRECTLY in Nearest Neighbor or Linear Interp mode: recording length of each (burst) trigger event   
                                               // parameter: duration
        
   //PARAMETER: endless, set to 1 to enable endless triggering, set to 0 and use count to acquire a certain # of trig events    
   
   if(!strcmp(argv[5],"true")){
		p_cfg->endless = true; 
   }else if(!strcmp(argv[5],"false")){
        p_cfg->endless = false;
   }else{
		printf("invalid endless parameter specified (true|false)\n");
	    exit(-1);
   }
       
   p_cfg->total_duration = atof(argv[6]); //time in seconds (used when endless is false) (this param isn't used when endless is true)    
       
   if(p_cfg->des_burst_duration == 0.0){
		printf("invalid des_burst_duration\n");
		exit(-1);
	}
		
	if( (!p_cfg->endless) && (p_cfg->total_duration == 0.0)){
			printf("invalid total_duration\n");
			exit(-1);
	}
		
	max_chunks_ptr = (int *) malloc(sizeof(int));
	print_info_ptr = (int *) malloc(sizeof(int));
		
	if( ((max_chunks_ptr) == NULL) || (print_info_ptr == NULL)){
			printf("malloc failed on max_chunks or print_info\n");
			exit(-1); 
	}
		
	(*max_chunks_ptr) = atoi(argv[7]);
		
	(*print_info_ptr) = atoi(argv[8]);
		
	//open log file
	
	if(*print_info_ptr){
			
		p_cfg->out_file = fopen("log.txt","w");
		if( p_cfg->out_file == NULL){
				printf("Error! Could not open file\n");
				exit(-1);
		}	
	}
		
    p_cfg->num_cols = (int) ceil((p_cfg->des_module_sampling_rate)*(p_cfg->des_burst_duration)); 
    p_cfg->num_bursts = (int) ceil((p_cfg->total_duration)/(p_cfg->des_burst_duration)); 
    p_cfg->num_rows = 4*(*max_chunks_ptr); // 1 demod multiply by 2 , 2 demods multiply by 4
		
	return; 
}

//----------------------------------------------------------------------------------------------------------------
static void connect_to_HF2LI(void *p_arg){
	
	//connect to the HF2LI and initialize the demodulators and outputs 
	
	ZIDAQCfgStruct *p_cfg = (ZIDAQCfgStruct *) p_arg;
	
	// Initialize ZIConnection.
    checkError(ziAPIInit(&(p_cfg->conn)),"Can't Init Connection");
      
    // Connect to the Data Server: Use port 8005 for the HF2 Data Server
    //HF2 only support ZI_API_VERSION_1, see the LabOne Programming Manual
    // for an explanation of API Levels.
    p_cfg->serverAddress = "localhost";
    p_cfg->retVal = ziAPIConnectEx(p_cfg->conn, p_cfg->serverAddress, 8005, ZI_API_VERSION_1, NULL);
        if (p_cfg->retVal != ZI_INFO_SUCCESS) {
                ziAPIGetError(p_cfg->retVal, &(p_cfg->errBuffer), NULL);
                fprintf(stderr, "Error, can't connect to the Data Server: `%s`.\n", p_cfg->errBuffer);
                exit(-1);
        }
        
    //CONNECTION ESTABLISHED! 
    //============================= Set Demod Rates =====================================//
    //Enable and set demod 1 sample rate 
    checkError(ziAPISetValueI(p_cfg->conn, "/dev1418/demods/0/enable", 1),"Can't enable demod");
    checkError(ziAPISetValueD(p_cfg->conn, "/dev1418/demods/0/rate", p_cfg->demod_sampling_rate),"Can't set demod rate"); //will round to nearest allowable value
        
    //Enable and set demod 4 sample rate
    checkError(ziAPISetValueI(p_cfg->conn, "/dev1418/demods/3/enable", 1),"Can't enable demod");
    checkError(ziAPISetValueD(p_cfg->conn, "/dev1418/demods/3/rate", p_cfg->demod_sampling_rate),"Can't set demod rate"); //will round to nearest allowable value
     
    //set output ranges: options 0.01 V, 0.1V, 1V, 10V ranges 
    checkError(ziAPISetValueD(p_cfg->conn, "/dev1418/sigouts/0/range", 1),"Can't set sigout range");
    checkError(ziAPISetValueD(p_cfg->conn, "/dev1418/sigouts/1/range", 1),"Can't set sigout range");
    
    //set output amplitudes  
    //enable output amplitudes
    // turn on outputs
    
    //Disable all other demodulators
    checkError(ziAPISetValueI(p_cfg->conn, "/dev1418/demods/1/enable", 0),"Can't enable demod");
    checkError(ziAPISetValueI(p_cfg->conn, "/dev1418/demods/2/enable", 0),"Can't enable demod");
       
    checkError(ziAPISetValueI(p_cfg->conn, "/dev1418/demods/4/enable", 0),"Can't enable demod");
    checkError(ziAPISetValueI(p_cfg->conn, "/dev1418/demods/5/enable", 0),"Can't enable demod");
            
    checkError(ziAPIGetValueI(p_cfg->conn,"dev1418/clockbase",(&(p_cfg->clockbase))),"Can't get clockbase");
    p_cfg->clockbase_d = (double) p_cfg->clockbase;
	if(*print_info_ptr){
	   fprintf(p_cfg->out_file,"Clockbase is: %f\n",p_cfg->clockbase_d); 
	 }

   return; 	
	
}

//-------------------------------------------------------------------------------------------------------------------

double timeSince(clock_t t1){
       //computes the time difference in seconds between processor clock time t1 and NOW
	   
	   clock_t diff = (clock() - t1);
	   return (((double) diff)/CLOCKS_PER_SEC);
	   
}
