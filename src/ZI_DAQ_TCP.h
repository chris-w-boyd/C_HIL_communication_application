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

double timeSince(clock_t t1);

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

//#define NUM_COLS 360
//#define NUM_ROWS 100

volatile sig_atomic_t cleanup_exit = 0;

void sigintHandler(int sig_num){
	cleanup_exit = 1;
}

void readData(ZIConnection* conn_ptr,ZIModuleHandle* DAQhdl_ptr, char *demod_sig_paths[], ZIModuleEventPtr* ev_ptr, char *path, int *sockfd_ptr, double **demodData,double **timeData,double clockbase_d, FILE *out_file);

int connectServer(const char* hostname, int portno);

static int readCount = 0; //keep track of number of calls to ziAPIModRead() 

static ZITimeStamp timeStamp0 = 0; //log the first timestamp

static int *max_chunks_ptr; 

static int *print_info_ptr;  
