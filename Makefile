exe: ZI_DAQ_TCP.c
	gcc ZI_DAQ_TCP.c -lm -L/opt/zi/LabOne64-18.12.60796/API/C/lib -Wl,-rpath=/opt/zi/LabOne64-18.12.60796/API/C/lib -Wall -o ZI_DAQ_TCP_v1 \
	-lziAPI-linux64 -lpthread

debug: ZI_DAQ_TCP.c
	gcc ZI_DAQ_TCP.c -lm -L/opt/zi/LabOne64-18.12.60796/API/C/lib -Wl,-rpath=/opt/zi/LabOne64-18.12.60796/API/C/lib -Wall -o ZI_DAQ_TCP_v1 \
	-g -lziAPI-linux64 -lpthread
	
clean: 
	rm ZI_DAQ_TCP
	
delete_log: 
	rm log.txt
	
	
	
