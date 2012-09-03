/*
wmr_hidprobe.c Oregon Scientific WMR100 reader using hiddev

$gcc -O3 -Wall -o wmr_hidprobe wmr_hidprobe.c

walkure at 3pf.jp
*/

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <time.h>
#include <getopt.h>

#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/hiddev.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include <sys/epoll.h>

#define VENDOR_ID 0x0fde
#define PRODUCT_ID 0xca01

#define LISTEN_PORT 13254

#define _countof(array) (sizeof(array)/sizeof(array[0]))
//#define MAKEWORD(low, high) ((__u16)((((__u16)(high)) << 8) | ((__u8)(low))))

typedef struct _WMR100TEMPHUMID
{
	__u8 bUnknown;		//0
	__u8 bType;			//1
	__u8 bSmileTrendId;	//2
	__u8 bTempLow;		//3
	__u8 bTempHighSign;	//4
	__u8 bHumid;		//5
	__u8 bDewLow;		//6
	__u8 bDewHighSign;	//7
	__u16 wUnknown;		//8,9
	__u16 wChecksum;    //10,11
//	__u8 bChecksumLow;	//10
//	__u8 bChecksumHigh;	//11
}__attribute__ ((packed)) WMR100TEMPHUMID;

typedef struct _WMR100TIMESTAMP
{
	__u8 bPowerInfo;	//0
	__u8 bType;			//1
	__u8 bUnknown1[2];	//2,3
	__u8 bMin;			//4
	__u8 bHour;			//5
	__u8 bDay;			//6
	__u8 bMonth;		//7
	__u8 bYear;			//8
	__u8 bUnknown2;		//9
	__u16 wChecksum;    //10,11
//	__u8 bChecksumLow;	//10
//	__u8 bChecksumHigh;	//11
}__attribute__ ((packed)) WMR100TIMESTAMP;

typedef struct _TEMPHUMID
{
	float _temp;
	int _humid;
} TEMPHUMID;

int open_device();
int send_report(int fd,__s32 packet[]);

void send_init(int fd);
int send_ready(int fd);

float make_tempvalue(__u8 bLow,__u8 bHighSign);
int handle_temp(WMR100TEMPHUMID *temp,TEMPHUMID *data);
time_t handle_time(WMR100TIMESTAMP *stamp);
void handle_unknown(__u8 buf[],int len);

int crack_chunk(__u8 buf[],int len,TEMPHUMID *data,time_t *tim);
int read_report(int fd,__s32 packet[]);
void read_hidd(int hidd,TEMPHUMID *data,time_t *tim);

int creat_listensock(int listen_port);
void send_response(int listend,TEMPHUMID *data,time_t tim);

//true main
int _main(int listen_port)
{
	int hidd,sockd=0,epd;
	TEMPHUMID data[2] = {{0}};
	time_t tim = 0;
	struct epoll_event ev;
    struct epoll_event events[2];
	
	if((hidd = open_device()) < 0){
		perror("cannot find device");
		return -1;
	}
	
	if((sockd = creat_listensock(listen_port)) < 0){
		perror("cannot creat sock");
		close(hidd);
		return -1;
	}
	
	if((epd = epoll_create(2)) < 0){
		perror("cannot creat epoll");
		close(hidd);
		close(sockd);
		return -1;
	}
	
	send_init(hidd);
	send_ready(hidd);
	printf("WMR100 initialized\n");
	
	memset(&ev, 0, sizeof ev);
	ev.events = EPOLLIN;
	
	ev.data.fd = hidd;
	if(epoll_ctl(epd, EPOLL_CTL_ADD, hidd, &ev) < 0){
		perror("cannot add hidd to epoll");
		close(epd);
		close(hidd);
		close(sockd);
		return -1;
	}
	
	ev.data.fd = sockd;
	if(epoll_ctl(epd, EPOLL_CTL_ADD, sockd, &ev) < 0){
		perror("cannot add sockd to epoll");
		close(epd);
		close(hidd);
		close(sockd);
		return -1;
	}
	
	
	for (;;) {
		int i;
	 	int evs = epoll_wait(epd, events, 2, -1);
	 	if(evs < 0){
	 		perror("epoll wait failured");
	 		break;
	 	}
	 	
	 	for (i = 0; i < evs; i++) {
	 		if(events[i].data.fd == hidd)
		 		read_hidd(hidd,data,&tim);
	 	
	 		if(events[i].data.fd == sockd)
		 		send_response(sockd,data,tim);
	 	}
	}
	
	close(epd);
	close(hidd);
	close(sockd);
	
	return 0;
}


int open_device()
{
	char devname[64];
	int i,fd,flag;
	struct hiddev_devinfo devinfo;
	
	for(i=0;i<16;i++){
		sprintf(devname,"/dev/usb/hiddev%d",i);
		
		fd = open(devname,O_RDWR);
		if(fd < 0){
			continue;
		}
		
		if (ioctl(fd, HIDIOCGDEVINFO, &devinfo) < 0) {
			close(fd);
			continue;
	    }
		
		if(((devinfo.vendor  & 0xffff) == VENDOR_ID) && ((devinfo.product & 0xffff) == PRODUCT_ID)){
			flag = HIDDEV_FLAG_UREF | HIDDEV_FLAG_REPORT;
			if (ioctl(fd, HIDIOCSFLAG, &flag) < 0) {
				perror("HIDIOCSFLAG");
				exit(1);
			}
			printf("Found WMR100 at %s\n",devname);
			return fd;
		}
		close(fd);
	}
	
	return -1;
}

int send_report(int fd,__s32 packet[])
{
	struct hiddev_usage_ref_multi urefs;
	struct hiddev_report_info     repo;
	
	int i;
	
	memset(&urefs, 0, sizeof(urefs));
	urefs.uref.report_type = HID_REPORT_TYPE_OUTPUT;
	urefs.uref.report_id   = 0;
	urefs.uref.field_index = 0;
	urefs.uref.usage_code  = 0xff000001;
	urefs.num_values       = 8;
	
	for(i=0;i<8;i++){
		urefs.values[i] = packet[i];
	}
	
	if (ioctl(fd, HIDIOCSUSAGES, &urefs) < 0) {
		perror("HIDIOCSUSAGES");
		exit(1);
	}
	
	memset(&repo, 0, sizeof(repo));
	repo.report_type = HID_REPORT_TYPE_OUTPUT;
	repo.report_id   = 0;
	repo.num_fields  = 1;

	if (ioctl(fd, HIDIOCSREPORT, &repo) < 0) {
		perror("HIDIOCSREPORT");
		exit(1);
	}
	
	return 0;
}

void send_init(int fd)
{
	__s32 ready_packet[] = {0x20, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00 };
	send_report(fd,ready_packet);
}
int send_ready(int fd)
{
	__s32 ready_packet[] = {0x01, 0xd0, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00 };
	return send_report(fd,ready_packet);
}

float make_tempvalue(__u8 bLow,__u8 bHighSign)
{
	__s16 temp = (bHighSign & 0x0f)* 0xff + bLow;
	
	if ( (bHighSign >> 4) == 0x08)
		temp = temp * -1;
	
	return temp/10.0f;
}

int handle_temp(WMR100TEMPHUMID *temp,TEMPHUMID *data)
{
	__u8 id=temp->bSmileTrendId & 0x0f;
	float tmpval = make_tempvalue(temp->bTempLow,temp->bTempHighSign);
	
	printf("Sensor[%d]/Tmp:%.01f,Humid:%d\n",id,tmpval,temp->bHumid);
	if(id == 0 || id == 1){
		data[id]._temp = tmpval;
		data[id]._humid = temp->bHumid;
	}
	return 0;
}

time_t handle_time(WMR100TIMESTAMP *stamp)
{
	struct tm tim = {0};
	
	printf("Time:%d/%02d/%02d %02d:%02d\n",
		stamp->bYear+2000,stamp->bMonth,stamp->bDay,stamp->bHour,stamp->bMin);
	
	tim.tm_year = stamp->bYear+2000-1900;
	tim.tm_mon = stamp->bMonth-1;
	tim.tm_mday = stamp->bDay;
	tim.tm_hour = stamp->bHour;
	tim.tm_min = stamp->bMin;
	
	return mktime(&tim);
}

void handle_unknown(__u8 buf[],int len)
{
	int i;
	printf("Unknown packet(%u):",len);
	for(i=0;i<len;i++)
		printf("%02x ",buf[i]);
	printf("\n");
}

int crack_chunk(__u8 buf[],int len,TEMPHUMID *data,time_t *tim)
{
	int i=0;
	__u16 sum = 0;
	WMR100TEMPHUMID temp;
	WMR100TIMESTAMP stamp;
	
	for(i=0;i<len-2;i++)
		sum += buf[i];
	
	switch(buf[1]){
	//handle temperature and humidity
	case 0x42:
		if(len < sizeof(temp)){
			return -1;
		}
		memcpy(&temp,buf,sizeof(temp));
		
		if(temp.wChecksum != sum){
			printf("Temp:Checksum Error 0x%d(current) != 0x%x(expected)\n",sum,temp.wChecksum);
//		if(MAKEWORD(temp.bChecksumLow,temp.bChecksumHigh) != sum){
//			printf("Temp:Checksum Error 0x%d(current) != 0x%x(expected)\n",sum,MAKEWORD(temp.bChecksumLow,temp.bChecksumHigh));
//			return -1;
		}
		printf("Packets(%d):",len);
		for(i=0;i<len;i++)
			printf("%02x ",buf[i]);
		printf("\n");
		return handle_temp(&temp,data);
		
	//handle timestamp
	case 0x60:
		if(len < sizeof(stamp)){
			return -1;
		}
		
		memcpy(&stamp,buf,sizeof(stamp));
		
		if(stamp.wChecksum != sum){
			printf("Stamp:Checksum Error 0x%d(current) != 0x%x(expected)\n",sum,stamp.wChecksum);
//		if(MAKEWORD(stamp.bChecksumLow,stamp.bChecksumHigh) != sum){
//			printf("Stamp:Checksum Error 0x%d(current) != 0x%x(expected)\n",sum,MAKEWORD(stamp.bChecksumLow,stamp.bChecksumHigh));
//			return -1;
		}
		
		printf("Packets(%d):",len);
		for(i=0;i<len;i++)
			printf("%02x ",buf[i]);
		printf("\n");
		return (*tim = handle_time(&stamp));
	default:
		handle_unknown(buf,len);
		return -1;
	};
	
}
int read_report(int fd,__s32 packet[])
{
	struct hiddev_report_info     repo;
	struct hiddev_usage_ref_multi urefs;
	int i;
	
	memset(&repo, 0, sizeof(repo));
	repo.report_type = HID_REPORT_TYPE_INPUT;
	repo.report_id   = 0;
	repo.num_fields  = 1; 

	if (ioctl(fd, HIDIOCGREPORT, &repo) < 0) {
		perror("HIDIOCGREPORT");
		exit(1);
	}

	memset(&urefs, 0, sizeof(urefs));
	urefs.uref.report_type = HID_REPORT_TYPE_INPUT;
	urefs.uref.report_id   = 0;
	urefs.uref.field_index = 0;
	urefs.uref.usage_code  = 0xff000001;
	urefs.num_values       = 8;

	if (ioctl(fd, HIDIOCGUSAGES, &urefs) < 0) {
		perror("HIDIOCGUSAGES");
		exit(1);
	}
	
	//read 8 bytes
	printf("Received:");
	for(i=0;i<8;i++){
		packet[i] = urefs.values[i];
		printf("%02x ",packet[i]);
	}
	puts("");
	
	//first byte is packet valid length
	if(packet[0] > 7)
		packet[0] = 7;
	
	return 0;
}

void read_hidd(int hidd,TEMPHUMID *data,time_t *tim)
{
	struct hiddev_usage_ref uref;
	__s32 packet[8];
	
	static __u8 chunk[256];
	static int len;
	
	int i;
	
	if (read(hidd, &uref, sizeof(uref)) < sizeof(uref)) {
		perror("read");
		exit(1);
	}

///	printf("[#%x] field#%x, usage#%x (0x%08x): %d\n",uref.report_id,uref.field_index,uref.usage_index,uref.usage_code,uref.value);
	
	//all data can be received?
	if (uref.field_index != HID_FIELD_INDEX_NONE)
//	if (uref.usage_index != 7)
		return;

	read_report(hidd,packet);
	
 	for(i=1 ; i <= packet[0] ; i++){
		if(packet[i] == 0xff){
			if(len){
				crack_chunk(chunk,len,data,tim);
				send_ready(hidd);
			}
			len=0;
			continue;
		}
		chunk[len] = packet[i];
		len ++;
	}
	
	if(len > 200){
		printf("invalid data\nignore!");
		len = 0;
	}
}

int creat_listensock(int listen_port)
{
	int sockd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	int flags = 1;
	
	if (sockd < 0) {
		perror("socket");
		return -1;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(listen_port);
	addr.sin_addr.s_addr = INADDR_ANY;
	
	setsockopt(sockd,SOL_SOCKET, SO_REUSEADDR, (const char *)&flags, sizeof(flags));

	//bind address
	if(bind(sockd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("bind");
		close(sockd);
		return -1;
	}

	//begin listening
	if (listen(sockd, 5) != 0) {
		perror("listen");
		close(sockd);
		return -1;
	}
	
	return sockd;
}

void send_response(int listend,TEMPHUMID *data,time_t tim)
{
	struct sockaddr_in client;
	socklen_t len = sizeof(client);
	int sock = accept(listend, (struct sockaddr *)&client, &len);
	char buf[245];
	
	snprintf(buf, sizeof(buf),
			"HTTP/1.0 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"\r\n"
			"time,%u\n"
			"sensor,0,%.01f,%d\n"
			"sensor,1,%.01f,%d\n"
		,(unsigned int)tim,data[0]._temp,data[0]._humid,data[1]._temp,data[1]._humid
	);
	
	if (sock < 0) {
		perror("accept");
		return;
	}
	
	send(sock, buf, (int)strlen(buf), 0);
	close(sock);
	
}

//entry point
int main(int argc,char **argv)
{
	int listen_port = LISTEN_PORT,opt,daemonize = 0;
	
	while((opt = getopt(argc,argv,"dl:p:")) != -1){
		switch(opt){
		case 'd':
			daemonize =1;
			break;
		case 'l':
		case 'p':
			listen_port = atoi(optarg);
			if(listen_port < 0 || listen_port > 65536){
				printf("invalid port spec:[%d]\n",listen_port);
				return -1;
			}
			break;
		default:
			printf("Usage: %s [-d][-p listen_port][-l listen_port](%c)\n",argv[0],opt);
			return -1;
		};
	}
	
	printf("listen port:%d\n",listen_port);
	
	if(daemonize)
	{
		printf("starting up as a daemon\n");
		
		if(daemon(0,0) < 0){
			perror("cannot daemonize");
			return -1;
		}
	}
	return _main(listen_port);
}
